/*
 * aether-init — AetherOS 的原生 1 号进程（PID 1）
 *
 * 背景（对应 AetherOS.md Phase 1 与 Architecture Audit P0-2）：
 *   在此之前，AetherOS Linux 用户空间的 PID 1 是 busybox init（/sbin/init 软链），
 *   能力局限于 inittab 的 sysinit / respawn / ctrlaltdel / shutdown 四类动作：
 *   没有服务依赖、没有崩溃监管、没有 AetherOS 语义、也没有关机信号契约。
 *
 * 本文件提供 AetherOS 自己的 init：
 *   1) 挂载虚拟文件系统（proc / sysfs / devtmpfs / devpts / run / tmp）
 *      —— 对应 AetherOS.md 中"虚拟文件系统层"在 Linux 上的原生形态；
 *   2) 设置主机名与全局环境（PATH / HOME / TERM ...）；
 *   3) 解析 /etc/aether/services 声明式服务清单，按依赖拓扑序启动；
 *   4) 常驻监管：oneshot 同步等待退出码；daemon 崩溃后按 restart 策略拉起
 *      （带防抖窗口，连续快速崩溃达上限即标记 FAILED，不再无限重启）；
 *   5) 信号语义：SIGTERM/SIGINT = 优雅关机(poweroff)，SIGUSR1 = 优雅重启(reboot)，
 *      SIGHUP = 恢复(recovery)流程；
 *   6) PID 1 法定职责：收养并回收所有孤儿进程（内核把孤儿一律 reparent 到 PID 1）。
 *
 * 诚实声明（AetherOS.md 原则 7：不假实现冒充真实现）：
 *   - recovery 当前实现为"优雅停机 + 写 /run/aether-recovery.request 标记"，
 *     真正的 A/B 槽位切换属于 Phase 2（aether-recovery），此处不冒充已完成；
 *   - 服务经 /bin/sh -c 执行——与 busybox inittab 的 exec 语义一致，便于复用
 *     既有 rc 脚本；这是有意的兼容选择，不是"内核级进程模型"；
 *   - 清单缺失/解析失败时进入 emergency-shell（fork /bin/sh 并继续监管），
 *     绝不静默退出——PID 1 退出会导致 kernel panic；
 *   - 读不到的配置（清单、主机名文件）一律记日志并跳过，不编造。
 *
 * 交叉编译：aarch64-linux-gnu-gcc -static -O2 -o aether-init-aarch64 aether-init.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>

#include "aether-sandbox.h"   /* P1-2b: service sandbox */
#include "aether-log.h"       /* P2-13a: serialized logging to aether-logd */

/* ------------------------------------------------------------------ */
/* 常量                                                                 */
/* ------------------------------------------------------------------ */

#define MANIFEST_PATH   "/etc/aether/services"
#define HOSTNAME_PATH   "/etc/hostname"
#define LOG_PATH        "/run/aether-init.log"
#define RECOVERY_REQ    "/run/aether-recovery.request"
#define CRASH_SOCK      "/run/aether-crash.sock"          /* P2-13b: -> aether-crashd */
#define SERVICES_STATE  "/run/aether-services.state"      /* P2-13c: -> aether-watchdog */

#define MAX_SERVICES    32
#define MAX_DEPS         8
#define NAME_LEN        32
#define EXEC_LEN       256
#define LINE_LEN       512

/* 防抖：窗口内重启次数达上限 -> 判定服务反复崩溃，标记 FAILED 不再重启 */
#define FLAP_WINDOW_SEC 10
#define FLAP_MAX         5
/* 优雅停机等待上限（秒），超时后 SIGKILL */
#define STOP_TIMEOUT_SEC 10

/* 关机请求模式（由信号设置） */
#define SHUTDOWN_NONE   0
#define SHUTDOWN_POWER  1   /* SIGTERM / SIGINT */
#define SHUTDOWN_REBOOT 2   /* SIGUSR1 */
#define SHUTDOWN_RECOV  3   /* SIGHUP */

/* ------------------------------------------------------------------ */
/* 服务表                                                               */
/* ------------------------------------------------------------------ */

enum svc_state   { S_STOPPED, S_RUNNING, S_DONE, S_FAILED };
enum restart_pol { R_NO, R_ALWAYS };

struct service {
    char           name[NAME_LEN];
    char           exec[EXEC_LEN];              /* 完整命令行，经 /bin/sh -c 执行 */
    char           deps[MAX_DEPS][NAME_LEN];
    int            ndeps;
    int            oneshot;
    enum restart_pol restart;
    enum svc_state  state;
    pid_t          pid;                          /* 0 = 未运行 */
    int            started;                      /* 是否已尝试启动过一次 */
    int            exit_code;                    /* 最近一次退出码（取证用） */
    int            restarts;
    int            sandbox;                      /* P1-2b: 是否以沙箱运行 */
    time_t         flap[FLAP_MAX + 1];           /* 最近重启时间戳（防抖窗口） */
    int            nflap;
};

static struct service g_svc[MAX_SERVICES];
static int            g_nsvc = 0;

static struct service *g_order[MAX_SERVICES];    /* 启动顺序（拓扑序） */
static int            g_norder = 0;

static volatile sig_atomic_t g_shutdown = SHUTDOWN_NONE;

/* ------------------------------------------------------------------ */
/* 日志：同时写控制台与 /run/aether-init.log                             */
/* ------------------------------------------------------------------ */

static double init_uptime(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1;
    double up = -1;
    if (fscanf(f, "%lf", &up) != 1) up = -1;
    fclose(f);
    return up;
}

static void log_line(const char *fmt, ...) {
    va_list ap;
    char msg[LINE_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    double up = init_uptime();
    FILE *lf = fopen(LOG_PATH, "a");
    if (lf) {
        if (up >= 0) fprintf(lf, "[%7.2f] %s\n", up, msg);
        else         fprintf(lf, "[  n/a  ] %s\n", msg);
        fclose(lf);
    }
    if (up >= 0) fprintf(stderr, "[%7.2f] aether-init: %s\n", up, msg);
    else         fprintf(stderr, "[  n/a  ] aether-init: %s\n", msg);
    fflush(stderr);

    /* P2-13a: same record to aether-logd (client lib falls back to kmsg) */
    aether_log(AETHER_LOG_INFO, "%s", msg);
}

/* ------------------------------------------------------------------ */
/* P2-13b: abnormal-exit event -> aether-crashd                         */
/*                                                                     */
/* abnormal = killed by a signal, or non-zero exit code. Normal exits   */
/* (code=0) are not reported - the crash manager is not an exit-code    */
/* ledger. Event is a single-line datagram:                             */
/*   crash <svc> <pid> <code> <restarts> <uptime> <cmdline>             */
/* ------------------------------------------------------------------ */

static void report_crash(const struct service *s, pid_t dead_pid, int status)
{
    int abnormal = WIFSIGNALED(status) ||
                   (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    if (!abnormal) return;

    char code[32];
    if (WIFSIGNALED(status))
        snprintf(code, sizeof code, "sig:%d", WTERMSIG(status));
    else
        snprintf(code, sizeof code, "code:%d", WEXITSTATUS(status));

    double up = init_uptime();
    char event[LINE_LEN + EXEC_LEN];
    int n = snprintf(event, sizeof event, "crash %s %d %s %d %.2f %s",
                     s->name, (int)dead_pid, code, s->restarts,
                     up >= 0 ? up : -1.0, s->exec);
    if (n <= 0 || n >= (int)sizeof event) return;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, CRASH_SOCK, sizeof sa.sun_path - 1);
    ssize_t sent = sendto(fd, event, (size_t)n, 0,
                          (struct sockaddr *)&sa, sizeof sa);
    close(fd);
    if (sent == (ssize_t)n)
        log_line("crash report for '%s' delivered to aether-crashd", s->name);
    else
        log_line("crash report for '%s' not delivered (%s) - log keeps the record",
                 s->name, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* P2-13c: service state snapshot -> /run/aether-services.state         */
/*                                                                     */
/* one line per service: `<name> <pid> <state>`; aether-watchdog uses   */
/* it to locate monitored service pids. Rewritten on every state change */
/* (n<=32, cost negligible).                                           */
/* ------------------------------------------------------------------ */

static void write_services_state(void)
{
    FILE *f = fopen(SERVICES_STATE, "w");
    if (!f) return;
    for (int i = 0; i < g_nsvc; i++) {
        struct service *s = &g_svc[i];
        const char *st = "stopped";
        switch (s->state) {
        case S_RUNNING: st = "running"; break;
        case S_DONE:    st = "done";    break;
        case S_FAILED:  st = "failed";  break;
        default:        st = "stopped"; break;
        }
        fprintf(f, "%s %d %s\n", s->name, (int)s->pid, st);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

static void msleep(long ms) { usleep(ms * 1000); }

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

/* 去掉行内注释（# 之后）并 trim；返回处理后的指针 */
static char *strip_comment(char *s) {
    char *h = strchr(s, '#');
    if (h) *h = '\0';
    return trim(s);
}

static struct service *svc_find(const char *name) {
    for (int i = 0; i < g_nsvc; i++)
        if (strcmp(g_svc[i].name, name) == 0) return &g_svc[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 1) 虚拟文件系统 + 主机名 + 环境                                      */
/* ------------------------------------------------------------------ */

struct vfs_mount {
    const char *src, *tgt, *fs, *data;
};

/* 顺序有意义：/dev 必须先于 /dev/pts；/run 早于日志文件 */
static const struct vfs_mount g_vfs[] = {
    { "proc",     "/proc",    "proc",     NULL },
    { "sysfs",    "/sys",     "sysfs",    NULL },
    { "devtmpfs", "/dev",     "devtmpfs", NULL },
    { "devpts",   "/dev/pts", "devpts",   "gid=5,mode=620" },
    { "tmpfs",    "/run",     "tmpfs",    "mode=755" },
    { "tmpfs",    "/tmp",     "tmpfs",    "mode=1777" },
};

static void ensure_dir(const char *path) {
    /* 逐段 mkdir（rootfs 已含大部分目录，这里兜底） */
    char buf[LINE_LEN];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void mount_vfs(void) {
    for (size_t i = 0; i < sizeof g_vfs / sizeof g_vfs[0]; i++) {
        const struct vfs_mount *m = &g_vfs[i];
        ensure_dir(m->tgt);
        if (mount(m->src, m->tgt, m->fs, 0, m->data) == 0) {
            fprintf(stderr, "aether-init: mounted %s on %s\n", m->fs, m->tgt);
        } else if (errno == EBUSY || errno == EINVAL) {
            /* 已由内核/其他方式挂载（如内核 cmdline 指定）——如实记录，不视为致命 */
            fprintf(stderr, "aether-init: %s on %s already mounted or refused (%s)\n",
                    m->fs, m->tgt, strerror(errno));
        } else {
            fprintf(stderr, "aether-init: WARNING cannot mount %s on %s (%s)\n",
                    m->fs, m->tgt, strerror(errno));
        }
    }
}

static void setup_hostname(void) {
    FILE *f = fopen(HOSTNAME_PATH, "r");
    if (!f) {
        fprintf(stderr, "aether-init: %s missing, keeping kernel hostname\n", HOSTNAME_PATH);
        return;
    }
    char name[128];
    if (!fgets(name, sizeof name, f)) {
        fclose(f);
        return;
    }
    fclose(f);
    name[strcspn(name, "\n")] = '\0';
    if (name[0] && sethostname(name, strlen(name)) == 0)
        fprintf(stderr, "aether-init: hostname set to %s\n", name);
    else
        fprintf(stderr, "aether-init: WARNING sethostname(%s) failed (%s)\n",
                name, strerror(errno));
}

static void setup_env(void) {
    setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "linux", 1);
    setenv("AETHEROS", "1", 1);       /* 标记运行在 AetherOS 原生用户空间 */
}

/* 内核命令行调参：`aether_wdt_grace=<秒>` -> 导出 AETHER_WDT_GRACE_S。
 * 场景：纯软件模拟（QEMU TCG，无 KVM）的慢宿主上，compositor 的冷启动
 * （DRM 初始化 + modeset）可达数分钟，看门狗默认 120s 启动宽限期可能
 * 仍不够——宿主侧改 QEMU -append 即可放宽，无需重编译镜像。
 * 参数缺失/非法时静默保留看门狗自身默认值（诚实降级，不谎报）。 */
static void export_cmdline_tuning(void) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    fclose(f);
    const char *p = strstr(line, "aether_wdt_grace=");
    if (!p) return;
    p += sizeof "aether_wdt_grace=" - 1;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || v <= 0) {
        fprintf(stderr, "aether-init: aether_wdt_grace= invalid — ignored (watchdog default kept)\n");
        return;
    }
    char val[32];
    snprintf(val, sizeof val, "%ld", v);
    setenv("AETHER_WDT_GRACE_S", val, 1);
    fprintf(stderr, "aether-init: watchdog startup grace = %ss (kernel cmdline)\n", val);
}

/* ------------------------------------------------------------------ */
/* 2) 服务清单解析                                                      */
/* ------------------------------------------------------------------ */

static void svc_reset(struct service *s) {
    memset(s, 0, sizeof *s);
    s->state = S_STOPPED;
    s->pid = 0;
}

/*
 * 清单语法（/etc/aether/services，行式，# 注释，空行忽略）：
 *
 *   service <name>            开始一个服务定义
 *   exec <command line>       完整命令行（经 /bin/sh -c 执行）
 *   depends <a>[,<b>...]      依赖的服务名（可多次追加）
 *   restart always|no         崩溃后是否重启（默认 no）
 *   oneshot yes|no            是否一次性任务（默认 no）
 *
 * 未知指令：记日志并忽略（向后兼容，不中断启动）。
 */
static int parse_bool(const char *v, int *out) {
    if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1")) { *out = 1; return 0; }
    if (!strcmp(v, "no")  || !strcmp(v, "false")|| !strcmp(v, "0")) { *out = 0; return 0; }
    return -1;
}

static int parse_manifest(void) {
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) {
        log_line("manifest %s not found (%s) — emergency mode", MANIFEST_PATH, strerror(errno));
        return -1;
    }

    char line[LINE_LEN];
    struct service *cur = NULL;
    int lineno = 0;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = strip_comment(line);
        if (!*s) continue;

        char *sp = strchr(s, ' ');
        char *arg = NULL;
        if (sp) {
            *sp = '\0';
            arg = trim(sp + 1);
        }

        if (!strcmp(s, "service")) {
            if (!arg || !*arg) {
                log_line("%s:%d: 'service' without name — ignored", MANIFEST_PATH, lineno);
                continue;
            }
            if (g_nsvc >= MAX_SERVICES) {
                log_line("%s:%d: service table full (%d) — ignored", MANIFEST_PATH, lineno, MAX_SERVICES);
                cur = NULL;
                continue;
            }
            if (svc_find(arg)) {
                log_line("%s:%d: duplicate service '%s' — ignored", MANIFEST_PATH, lineno, arg);
                cur = NULL;
                continue;
            }
            cur = &g_svc[g_nsvc++];
            svc_reset(cur);
            snprintf(cur->name, sizeof cur->name, "%s", arg);
        } else if (!cur) {
            log_line("%s:%d: directive '%s' before any 'service' — ignored", MANIFEST_PATH, lineno, s);
            continue;
        } else if (!strcmp(s, "exec")) {
            if (!arg || !*arg) {
                log_line("%s:%d: 'exec' without command — ignored", MANIFEST_PATH, lineno);
                continue;
            }
            snprintf(cur->exec, sizeof cur->exec, "%s", arg);
        } else if (!strcmp(s, "depends")) {
            if (!arg || !*arg) continue;
            char *save = NULL;
            for (char *tok = strtok_r(arg, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                char *d = trim(tok);
                if (!*d) continue;
                if (cur->ndeps >= MAX_DEPS) {
                    log_line("%s:%d: service '%s' has too many depends (max %d) — '%s' ignored",
                             MANIFEST_PATH, lineno, cur->name, MAX_DEPS, d);
                    continue;
                }
                snprintf(cur->deps[cur->ndeps++], NAME_LEN, "%s", d);
            }
        } else if (!strcmp(s, "restart")) {
            /* 取值: always | no（也兼容 yes/true/1 与 no/false/0） */
            if (!arg) {
                log_line("%s:%d: 'restart' expects always|no — ignored", MANIFEST_PATH, lineno);
                continue;
            }
            if (!strcmp(arg, "always")) {
                cur->restart = R_ALWAYS;
            } else {
                int v;
                if (parse_bool(arg, &v) != 0) {
                    log_line("%s:%d: 'restart' expects always|no — ignored", MANIFEST_PATH, lineno);
                    continue;
                }
                cur->restart = v ? R_ALWAYS : R_NO;
            }
        } else if (!strcmp(s, "oneshot")) {
            int v;
            if (!arg || parse_bool(arg, &v) != 0) {
                log_line("%s:%d: 'oneshot' expects yes|no — ignored", MANIFEST_PATH, lineno);
                continue;
            }
            cur->oneshot = v;
        } else if (!strcmp(s, "sandbox")) {
            int v;
            if (!arg || parse_bool(arg, &v) != 0) {
                log_line("%s:%d: 'sandbox' expects yes|no — ignored", MANIFEST_PATH, lineno);
                continue;
            }
            cur->sandbox = v;
        } else {
            log_line("%s:%d: unknown directive '%s' — ignored", MANIFEST_PATH, lineno, s);
        }
    }
    fclose(f);

    /* 校验：依赖必须存在；exec 必须非空 */
    int ok = 1;
    for (int i = 0; i < g_nsvc; i++) {
        struct service *s = &g_svc[i];
        if (!s->exec[0]) {
            log_line("service '%s' has no exec — disabled", s->name);
            s->state = S_FAILED;
            s->started = 1;
            ok = 0;
        }
        for (int d = 0; d < s->ndeps; d++) {
            if (!svc_find(s->deps[d])) {
                log_line("service '%s' depends on unknown service '%s' — disabled",
                         s->name, s->deps[d]);
                s->state = S_FAILED;
                s->started = 1;
                ok = 0;
            }
        }
    }
    if (ok)
        log_line("manifest parsed: %d services", g_nsvc);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 3) 启动（依赖拓扑序）                                                */
/* ------------------------------------------------------------------ */

static const char *state_name(enum svc_state st) {
    switch (st) {
        case S_STOPPED: return "stopped";
        case S_RUNNING: return "running";
        case S_DONE:    return "done";
        case S_FAILED:  return "failed";
    }
    return "?";
}

/* 依赖是否已满足：
 *   oneshot 依赖方  -> 必须已成功跑完（DONE）
 *   daemon  依赖方  -> 必须已拉起且存活（RUNNING）
 * 两者任何一方 FAILED 都视为"依赖失败"（由调用方区分处理）。 */
static int dep_satisfied(const struct service *dep) {
    if (dep->oneshot) return dep->state == S_DONE;
    return dep->state == S_RUNNING;
}

/* fork + exec 一个服务；返回子 pid，失败返回 -1 */
static pid_t spawn_service(struct service *s) {
    pid_t pid = fork();
    if (pid < 0) {
        log_line("service '%s': fork failed (%s)", s->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 子进程：脱离 init 的会话，恢复默认信号处置 */
        setsid();
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        /* P1-2b: 若服务声明 sandbox，在 exec 前施加完整进程沙箱
         * （私有 mnt ns + 丢能力 + 非 root + seccomp 允许列表）。
         * 失败也只是缺少部分沙箱，绝不静默宣称成功。 */
        if (s->sandbox)
            aether_apply_sandbox(s->name);

        /* 确保 0/1/2 均有合法 fd（init 场景通常已是 /dev/console） */
        for (int fd = 0; fd < 3; fd++) {
            if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
                int n = open("/dev/console", O_RDWR);
                if (n != fd && n >= 0) { dup2(n, fd); close(n); }
            }
        }

        execl("/bin/sh", "sh", "-c", s->exec, (char *)NULL);
        fprintf(stderr, "aether-init: service '%s': exec failed: %s\n", s->name, strerror(errno));
        _exit(127);
    }
    return pid;
}

/* 同步等待一个 oneshot 服务退出；同时回收在此期间退出的其它子进程 */
static void handle_child(pid_t pid, int status); /* 定义见第 4 节 */
static int wait_oneshot(struct service *s, pid_t pid) {
    for (;;) {
        int st;
        pid_t w = waitpid(-1, &st, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            log_line("service '%s': waitpid failed (%s)", s->name, strerror(errno));
            return -1;
        }
        if (w == pid) {
            if (WIFEXITED(st)) {
                int code = WEXITSTATUS(st);
                s->state = (code == 0) ? S_DONE : S_FAILED;
                s->exit_code = code;
                log_line("oneshot '%s' exited code=%d -> %s",
                         s->name, code, state_name(s->state));
                return code;
            }
            if (WIFSIGNALED(st)) {
                s->state = S_FAILED;
                s->exit_code = 128 + WTERMSIG(st);
                log_line("oneshot '%s' killed by signal %d -> failed",
                         s->name, WTERMSIG(st));
                return -1;
            }
        } else {
            /* 其它子进程（含被收养的孤儿）在此期间退出：必须转交监管
             * 逻辑，不能只记录日志——否则在此窗口内崩溃的 daemon 会
             * 失去监管（既不重启，停机时还会向已死的 pid 发信号）。 */
            handle_child(w, st);
        }
    }
}

/* 尝试启动一个尚未启动的服务；已 FAILED 的不再启动 */
static void start_service(struct service *s) {
    if (s->started || s->state == S_FAILED) return;

    for (int d = 0; d < s->ndeps; d++) {
        struct service *dep = svc_find(s->deps[d]);
        if (!dep) continue; /* 解析期已记录 */
        if (dep->state == S_FAILED) {
            log_line("service '%s': dependency '%s' failed -> not started",
                     s->name, dep->name);
            s->state = S_FAILED;
            s->started = 1;
            return;
        }
        if (!dep_satisfied(dep)) return; /* 依赖未就绪，等下一轮 */
    }

    s->started = 1;
    if (s->oneshot) {
        log_line("starting oneshot '%s': %s", s->name, s->exec);
        pid_t pid = spawn_service(s);
        if (pid < 0) { s->state = S_FAILED; return; }
        s->pid = pid;
        s->state = S_RUNNING;
        write_services_state();
        wait_oneshot(s, pid);
        s->pid = 0;
        write_services_state();
    } else {
        log_line("starting service '%s': %s", s->name, s->exec);
        pid_t pid = spawn_service(s);
        if (pid < 0) { s->state = S_FAILED; return; }
        s->pid = pid;
        s->state = S_RUNNING;
        s->restarts = 0;
        s->nflap = 0;
        write_services_state();
        log_line("service '%s' running (pid=%d)", s->name, pid);
    }
}

/*
 * 依赖拓扑启动：反复扫描，启动"依赖已全部满足"的服务。
 * 这不是严格的 Kahn 队列，而是等价的贪心拓扑序（n<=32，O(n^2) 可忽略），
 * 语义一致且实现更直白。
 */
static void start_all(void) {
    int progress = 1, guard = 0;
    while (progress && guard++ < MAX_SERVICES + 2) {
        progress = 0;
        for (int i = 0; i < g_nsvc; i++) {
            struct service *s = &g_svc[i];
            if (s->started || s->state == S_FAILED) continue;
            int ready = 1;
            for (int d = 0; d < s->ndeps && ready; d++) {
                struct service *dep = svc_find(s->deps[d]);
                if (!dep || dep->state == S_FAILED) ready = 0;
                else if (!dep_satisfied(dep)) ready = 0;
            }
            if (ready) { start_service(s); progress = 1; }
        }
    }
    /* 未启动的即存在环或不可满足依赖 */
    for (int i = 0; i < g_nsvc; i++) {
        struct service *s = &g_svc[i];
        if (!s->started && s->state != S_FAILED) {
            log_line("service '%s': unresolved dependency (cycle?) -> failed", s->name);
            s->state = S_FAILED;
            s->started = 1;
        }
        g_order[g_norder++] = s; /* 记录顺序（停机时逆序使用） */
    }
}

/* ------------------------------------------------------------------ */
/* 4) 监管与重启                                                        */
/* ------------------------------------------------------------------ */

/* 防抖：窗口内重启次数达上限 -> 放弃 */
static int flap_exceeded(struct service *s, time_t now) {
    if (s->nflap < FLAP_MAX) return 0;
    /* 窗口内（从最早一次算起）是否已发生 FLAP_MAX 次重启 */
    return (now - s->flap[0]) < FLAP_WINDOW_SEC;
}

static void note_restart(struct service *s, time_t now) {
    if (s->nflap < (int)(sizeof s->flap / sizeof s->flap[0]))
        s->flap[s->nflap++] = now;
}

static void handle_child(pid_t pid, int status) {
    struct service *s = NULL;
    for (int i = 0; i < g_nsvc; i++)
        if (g_svc[i].pid == pid) { s = &g_svc[i]; break; }

    if (!s) {
        /* 不是任何已登记服务的子进程：孤儿回收（PID 1 法定职责） */
        log_line("reaped orphan pid=%d (status=0x%x)", pid, status);
        return;
    }

    int exited  = WIFEXITED(status);
    int how     = exited ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    pid_t dead_pid = s->pid;
    report_crash(s, dead_pid, status);   /* P2-13b: 异常退出 -> crashd */
    s->pid = 0;
    write_services_state();              /* P2-13c: 状态变化即快照 */

    if (s->oneshot) {
        /* oneshot 的退出码已在 wait_oneshot 中处理；这里兜底 */
        s->state = (exited && WEXITSTATUS(status) == 0) ? S_DONE : S_FAILED;
        return;
    }

    if (g_shutdown != SHUTDOWN_NONE) {
        s->state = S_STOPPED;
        return;
    }

    if (s->restart != R_ALWAYS) {
        log_line("service '%s' exited (code=%d, restart=no) -> stopped", s->name, how);
        s->state = S_STOPPED;
        return;
    }

    time_t now = time(NULL);
    note_restart(s, now);
    if (flap_exceeded(s, now)) {
        log_line("service '%s' crashed %d times within %ds -> giving up (failed)",
                 s->name, FLAP_MAX, FLAP_WINDOW_SEC);
        s->state = S_FAILED;
        return;
    }

    log_line("service '%s' exited (code=%d) -> restarting (#%d)",
             s->name, how, ++s->restarts);
    pid_t np = spawn_service(s);
    if (np < 0) { s->state = S_FAILED; write_services_state(); return; }
    s->pid = np;
    s->state = S_RUNNING;
    write_services_state();              /* P2-13c: watchdog 需要新 pid */
    log_line("service '%s' restarted (pid=%d)", s->name, np);
}

/* ------------------------------------------------------------------ */
/* 5) 信号与停机                                                        */
/* ------------------------------------------------------------------ */

static void on_signal(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:  g_shutdown = SHUTDOWN_POWER;  break;
        case SIGUSR1: g_shutdown = SHUTDOWN_REBOOT; break;
        case SIGHUP:  g_shutdown = SHUTDOWN_RECOV;  break;
        default: break;
    }
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;          /* 无 SA_RESTART：waitpid 返回 EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
}

/* 非阻塞回收一波子进程，返回是否还有存活服务 */
static int reap_quietly(void) {
    int alive = 0;
    for (;;) {
        int st;
        pid_t w = waitpid(-1, &st, WNOHANG);
        if (w > 0) {
            for (int i = 0; i < g_nsvc; i++)
                if (g_svc[i].pid == w) { g_svc[i].pid = 0; g_svc[i].state = S_STOPPED; break; }
        } else {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
    }
    for (int i = 0; i < g_nsvc; i++)
        if (g_svc[i].pid > 0) alive = 1;
    return alive;
}

static void write_recovery_request(void) {
    FILE *f = fopen(RECOVERY_REQ, "w");
    if (!f) {
        log_line("recovery: cannot write %s (%s)", RECOVERY_REQ, strerror(errno));
        return;
    }
    fprintf(f, "requested_at=%ld\nreason=SIGHUP to aether-init (PID 1)\n"
               "slot_switch=phase-2 (aether-recovery) — not implemented yet\n",
            (long)time(NULL));
    fclose(f);
    log_line("recovery: request marker written to %s", RECOVERY_REQ);
}

static void do_shutdown(int mode) {
    const char *modename =
        mode == SHUTDOWN_POWER  ? "poweroff" :
        mode == SHUTDOWN_REBOOT ? "reboot"   : "recovery";

    log_line("=== shutdown: %s ===", modename);

    if (mode == SHUTDOWN_RECOV)
        write_recovery_request();

    /* 逆依赖序发送 SIGTERM。
     * 每个服务 spawn 时都 setsid()，故 pid 即进程组 id；杀整组才能连同
     * 服务的子进程（如 getty 拉起 login shell）一并优雅停止。 */
    for (int i = g_norder - 1; i >= 0; i--) {
        struct service *s = g_order[i];
        if (s->pid > 0) {
            log_line("stopping service '%s' (pid=%d) SIGTERM", s->name, s->pid);
            kill(-s->pid, SIGTERM);
        }
    }

    /* 等待优雅退出 */
    for (int t = 0; t < STOP_TIMEOUT_SEC * 10; t++) {
        if (!reap_quietly()) break;
        msleep(100);
    }

    /* 超时未退：SIGKILL 整组 */
    for (int i = 0; i < g_nsvc; i++) {
        if (g_svc[i].pid > 0) {
            log_line("service '%s' (pid=%d) did not exit in %ds -> SIGKILL",
                     g_svc[i].name, g_svc[i].pid, STOP_TIMEOUT_SEC);
            kill(-g_svc[i].pid, SIGKILL);
        }
    }
    while (reap_quietly()) msleep(50);

    /* 逆序卸载我们挂载的虚拟文件系统（尽力而为，失败不影响停机） */
    for (int i = (int)(sizeof g_vfs / sizeof g_vfs[0]) - 1; i >= 0; i--) {
        if (umount2(g_vfs[i].tgt, 0) == 0)
            log_line("unmounted %s", g_vfs[i].tgt);
    }

    sync();

    if (mode == SHUTDOWN_REBOOT) {
        log_line("reboot: calling reboot(RB_AUTOBOOT)");
        sync();
        reboot(RB_AUTOBOOT);
    } else {
        /* poweroff 与 recovery 都走关机：
         * recovery 的槽位切换是 Phase 2 工作，此处只完成信号契约与标记。 */
        log_line("%s: calling reboot(RB_POWER_OFF)", modename);
        sync();
        reboot(RB_POWER_OFF);
    }

    /* reboot() 正常不会返回；若返回（异常环境）则挂起，PID 1 绝不退出 */
    log_line("reboot() returned unexpectedly — halting (PID 1 must not exit)");
    for (;;) pause();
}

/* ------------------------------------------------------------------ */
/* 6) 应急回退                                                          */
/* ------------------------------------------------------------------ */

/* 清单缺失/损坏：拉一个受监管的 shell，保证系统可用且 PID 1 不退出 */
static void emergency_shell(void) {
    log_line("EMERGENCY: spawning supervised /bin/sh (manifest unusable)");
    struct service *s = &g_svc[g_nsvc++];
    svc_reset(s);
    snprintf(s->name, sizeof s->name, "emergency-shell");
    snprintf(s->exec, sizeof s->exec, "%s", "/bin/sh");
    s->restart = R_ALWAYS;
    start_service(s);
    if (g_norder < MAX_SERVICES) g_order[g_norder++] = s;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    aether_log_init("aether-init");   /* P2-13a: logd 客户端身份 */

    /* PID 1 自检：只有 1 号进程才能承担这些职责 */
    if (getpid() != 1) {
        fprintf(stderr, "aether-init: refusing to run — not PID 1 (pid=%d)\n", getpid());
        return 1;
    }

    fprintf(stderr, "aether-init: AetherOS native init starting (PID 1)\n");

    /* 自证身份：execve 路径（/sbin/init）决定 comm，这里显式改名，
     * ps / /proc/1/comm 即显示 aether-init，与 busybox init(comm=init) 区分。 */
    prctl(PR_SET_NAME, "aether-init", 0, 0, 0);

    mount_vfs();          /* 1) 虚拟文件系统（/run 之后日志才可落盘） */
    setup_hostname();     /* 2) 主机名 */
    setup_env();          /* 3) 全局环境 */
    export_cmdline_tuning(); /* 4) 内核命令行调参 -> 子进程环境（看门狗宽限期等） */

    /* P1-2b 全局加固：禁用非特权用户命名空间。沙箱已用 seccomp 杀死
     * unshare/setns 并丢弃全部能力，但 clone3(CLONE_NEWUSER) 的标志在
     * seccomp 中无法直接检视（藏在指针后）。把 max_user_namespaces 置 0
     * 后，任何非特权 NEWUSER 克隆都会返回 EPERM，彻底堵死"克隆 user ns
     * 重获 root"的逃逸路径。此写操作需 CAP_SYS_ADMIN（当前为 PID 1 root）。 */
    {
        int fd = open("/proc/sys/user/max_user_namespaces", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "0\n", 2) == 2)
                fprintf(stderr, "aether-init: hardened user.max_user_namespaces=0\n");
            else
                fprintf(stderr, "aether-init: WARN cannot set max_user_namespaces (%s)\n",
                        strerror(errno));
            close(fd);
        } else {
            fprintf(stderr, "aether-init: WARN cannot open max_user_namespaces (%s)\n",
                    strerror(errno));
        }
    }

    install_signals();    /* 4) 信号契约 */

    log_line("aether-init online: vfs mounted, hostname set, signals armed");

    if (parse_manifest() != 0) {
        emergency_shell();
    } else {
        start_all();
        int nfailed = 0;
        for (int i = 0; i < g_nsvc; i++)
            if (g_svc[i].state == S_FAILED) nfailed++;
        log_line("service startup complete (%d services, %d failed)", g_nsvc, nfailed);
    }

    /* 监管循环：PID 1 在此常驻，直到收到关机信号 */
    log_line("entering supervision loop");
    for (;;) {
        if (g_shutdown != SHUTDOWN_NONE)
            do_shutdown(g_shutdown);

        int st;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;      /* 信号到达：回到循环顶部处理 */
            if (errno == ECHILD) {
                /* 没有子进程了（极端情况）：等一秒再探，绝不退出 */
                msleep(1000);
                continue;
            }
            log_line("waitpid: %s", strerror(errno));
            msleep(1000);
            continue;
        }
        handle_child(pid, st);
    }
}
