/*
 * aether-watchdog.c - AetherOS 软看门狗（P2-13c）。
 *
 * 监控对象：aetherosd 与 aether-compositor 的「活跳」——周期 5s 向两者
 * 的 IPC socket 发心跳请求并要求在 HB_TIMEOUT_MS（默认 5s）内得到响应：
 *   - aetherosd    : connect 后服务端 accept 即推状态快照（读即心跳）；
 *   - aether-compositor: connect 后发 "status"（单条消息、无换行——
 *     compositor 用 recvmsg 收单条消息，带 \n 反而破坏协议）。
 *
 * 连续 3 个周期无响应（进程活着但 IPC 僵死——init 的 waitpid 看不见
 * 这种死法）→ SIGKILL → aether-init 按 restart=always 拉起新进程
 * （boot-smoke H 段已证 init 的重启路径；本守护负责发现僵死）。
 *
 * 启动宽限期（GRACE_S，默认 120s，可用 AETHER_WDT_GRACE_S 覆盖）：
 * 纯软件模拟（QEMU TCG，无 KVM）的冷启动极慢——compositor 刻意先绑定
 * IPC socket 再做 DRM 初始化 + modeset（提前 bind，客户端连接排队等
 * 待而非 ECONNREFUSED），手机上这段可达数十秒；此期间「连接成功但
 * 无人应答」与僵死在 socket 层面无从区分。若从首个周期就计 miss，
 * 看门狗会把「慢启动的健康服务」当僵死反复处决，形成重启循环
 * （init 重启 -> 又赶不上 -> 再杀）。
 * 因此：新 pid 出现（或 watchdog 自身启动）后，心跳失败不计 miss，
 * 直到以下两者先到者：① 首次成功心跳（证明服务真的起来了——此后立
 * 即恢复 3-miss 监管）；② 宽限期上限 GRACE_S（始终不响的服务兜底）。
 *
 * 宽限期的唯一例外——进程被冻结（/proc/<pid>/stat 调度状态 T/t，即
 * SIGSTOP/SIGTSTP/tracing stop）：被冻住的进程一步都不走，不可能
 * 「正在启动」；「连上 socket 却永不应答」+「调度状态为停止」= 明确
 * 的僵死，照计 miss，不等宽限期。这覆盖真实时序：init 刚把服务重启
 * 起来（boot-smoke H 段 kill -9 -> 新 pid），数秒后即被冻结（W 段
 * SIGSTOP）——新 pid 尚未完成首次成功心跳，若无本例外，宽限会把僵死
 * 白放 120s。compositor 冷启动的「连上但无声」其状态是 R/D 而非 T，
 * 不会被误判（重启循环不会回归）。
 * 心跳超时 HB_TIMEOUT_MS 默认 5s（原 2s）：TCG 下客机被宿主饿住时，
 * 健康的 compositor 也可能超过 2s 才应答——放宽到 5s 防误判，
 * 仍远小于 3×5s 的处决窗口，检测能力不受实质影响。
 *
 * 硬件看门狗：若内核提供 /dev/watchdog（QEMU 多数配置不提供），打开并
 * 每周期喂狗；缺席时如实记录——本场景是软看门狗，不冒充硬件狗。
 *
 * 诚实边界：
 *   - 不监控 PID 1（aether-init 退出 → 内核 panic，由内核兜底）；
 *   - 不监控 oneshot 服务（pkginstall/bootdiag 本就退出）；
 *   - 心跳成功≠功能正确，只证明 IPC 路径活着（不夸大）。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "aether-log.h"

#define SERVICES_STATE  "/run/aether-services.state"
#define AETHEROSD_SOCK  "/run/aetheros.sock"
#define COMP_SOCK       "/run/aetheros-comp.sock"
#define WDT_DEV         "/dev/watchdog"

#define PERIOD_S        5
#define HB_TIMEOUT_MS   5000
#define MISS_LIMIT      3
#define GRACE_S         120     /* 新 pid 启动宽限期：慢 TCG 冷启动不误杀 */

struct target {
    const char *name;
    const char *sock;
    int send_status;      /* compositor 需要显式发 "status" */
    int miss;
    pid_t pid;
    time_t first_seen;    /* 当前 pid 首次被观测到的时刻（宽限期起点） */
    int grace_logged;     /* 宽限期内只登记一次，避免日志刷屏 */
    int proved;           /* 是否已成功心跳过一次（活过 -> 宽限期提前结束） */
};

static struct target g_targets[] = {
    { "aetherosd",    AETHEROSD_SOCK, 0, 0, -1, 0, 0, 0 },
    { "compositor",   COMP_SOCK,      1, 0, -1, 0, 0, 0 },
};

static volatile sig_atomic_t g_stop = 0;
static int g_wdt_fd = -1;
static int g_grace_s = GRACE_S;
static void on_sigterm(int sig) { (void)sig; g_stop = 1; }

/* 环境变量覆盖（默认值诚实写在宏里）：AETHER_WDT_GRACE_S 可在极慢的
 * 宿主上进一步放宽启动宽限期。 */
static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    if (v && *v) {
        int n = atoi(v);
        if (n > 0) return n;
    }
    return def;
}

/* 从 init 的服务状态文件取 pid：`<name> <pid> <state>`。 */
static pid_t lookup_pid(const char *name)
{
    FILE *f = fopen(SERVICES_STATE, "r");
    if (!f) return -1;
    char line[256];
    pid_t pid = -1;
    while (fgets(line, sizeof line, f)) {
        char n[64]; int p; char st[32];
        if (sscanf(line, "%63s %d %31s", n, &p, st) == 3) {
            if (!strcmp(n, name)) { pid = (pid_t)p; break; }
        }
    }
    fclose(f);
    return pid;
}

/* 心跳：connect -> （按需发 status）-> HB_TIMEOUT_MS 内读到任意字节即活。 */
static int heartbeat(struct target *t)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, t->sock, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }

    if (t->send_status) {
        /* 无换行：compositor recvmsg 收单条消息（与 demo/aethercompctl 同协议） */
        ssize_t w = write(fd, "status", 6);
        (void)w;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, HB_TIMEOUT_MS);
    int alive = 0;
    if (pr == 1 && (pfd.revents & POLLIN)) {
        char buf[256];
        ssize_t got = read(fd, buf, sizeof buf - 1);
        alive = (got > 0);
    }
    close(fd);
    return alive ? 0 : -1;
}

/* 进程调度状态探测：/proc/<pid>/stat 第 3 字段（comm 之后的单字符）。
 * 为 T（SIGSTOP 停止）或 t（tracing stop）时返回 1——进程被外部冻住，
 * 一步都不走，不可能「正在启动」。读失败（进程已退出等）返回 0：无法
 * 证明冻结，按普通心跳失败处理，不冤枉。 */
static int proc_frozen(pid_t pid)
{
    char path[64], buf[512];
    FILE *f;
    size_t n;
    char *rp;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    rp = strrchr(buf, ')');          /* 跳过可能含空格/括号的 comm */
    if (!rp || rp[1] != ' ' || rp[2] == '\0') return 0;
    return rp[2] == 'T' || rp[2] == 't';
}

int main(void)
{
    aether_log_init("aether-watchdog");
    g_grace_s = env_int("AETHER_WDT_GRACE_S", GRACE_S);
    signal(SIGTERM, on_sigterm);
    signal(SIGINT,  on_sigterm);
    signal(SIGPIPE, SIG_IGN);

    /* 硬件看门狗：能开则喂，开不得如实降级软看门狗。 */
    g_wdt_fd = open(WDT_DEV, O_WRONLY);
    if (g_wdt_fd >= 0) {
        aether_log(AETHER_LOG_INFO, "watchdog: hardware watchdog %s open — feeding each period", WDT_DEV);
    } else {
        aether_log(AETHER_LOG_INFO,
                   "watchdog: %s unavailable (%s) — software watchdog only "
                   "(service-level recovery; NOT a hardware reset path)",
                   WDT_DEV, strerror(errno));
    }

    aether_log(AETHER_LOG_INFO,
               "watchdog: monitoring %d services, period=%ds, miss_limit=%d, "
               "startup_grace=%ds, hb_timeout=%dms",
               (int)(sizeof g_targets / sizeof g_targets[0]), PERIOD_S,
               MISS_LIMIT, g_grace_s, HB_TIMEOUT_MS);

    while (!g_stop) {
        if (g_wdt_fd >= 0) {
            ssize_t w = write(g_wdt_fd, "1", 1);   /* 喂狗 */
            (void)w;
        }
        time_t now = time(NULL);
        for (size_t i = 0; i < sizeof g_targets / sizeof g_targets[0]; i++) {
            struct target *t = &g_targets[i];
            pid_t pid = lookup_pid(t->name);
            if (pid != t->pid) {
                /* 新 pid（watchdog 启动后首次见到，或 init 重启后的新进程）：
                 * 重置计数并开启启动宽限期——慢 TCG 冷启动期间不误杀。 */
                t->pid = pid;
                t->miss = 0;
                t->first_seen = now;
                t->grace_logged = 0;
                t->proved = 0;
                if (pid < 0)
                    continue;   /* 服务未启动/已退出——归 init 管 */
                aether_log(AETHER_LOG_INFO,
                           "watchdog: %s (pid=%d) under observation — "
                           "startup grace until first heartbeat (cap %ds)",
                           t->name, (int)pid, g_grace_s);
            }
            if (t->pid < 0) {
                /* 服务未启动/已退出——归 init 管，不重复杀 */
                t->miss = 0;
                continue;
            }
            if (heartbeat(t) == 0) {
                t->proved = 1;   /* 活过：宽限期提前结束，之后按正常监管 */
                if (t->miss)
                    aether_log(AETHER_LOG_INFO, "watchdog: %s (pid=%d) responsive again", t->name, (int)t->pid);
                t->miss = 0;
                continue;
            }
            if (!t->proved && now - t->first_seen < g_grace_s) {
                /* 启动宽限期内：心跳失败不计 miss（慢启动 ≠ 僵死）。
                 * 首次成功心跳后 proved=1，本分支不再进入。
                 * 唯一例外：进程被冻结（state T/t）——冻住的进程不可能
                 * 「正在启动」，连上 socket 却永不应答即僵死，照计
                 * miss：不等整个宽限期（覆盖 init 重启后数秒即被冻住
                 * 的真实时序，boot-smoke W3 正是这一场景）。 */
                if (proc_frozen(t->pid)) {
                    aether_log(AETHER_LOG_WARN,
                               "watchdog: %s (pid=%d) frozen (state T) — "
                               "counting miss despite startup grace "
                               "(frozen != slow start)",
                               t->name, (int)t->pid);
                } else {
                    if (!t->grace_logged) {
                        aether_log(AETHER_LOG_INFO,
                                   "watchdog: %s (pid=%d) not responsive yet — "
                                   "within startup grace (%ld/%ds), not counting "
                                   "(slow cold start tolerated)",
                                   t->name, (int)t->pid,
                                   (long)(now - t->first_seen), g_grace_s);
                        t->grace_logged = 1;
                    }
                    continue;
                }
            }
            t->miss++;
            aether_log(AETHER_LOG_WARN, "watchdog: %s (pid=%d) heartbeat missed (%d/%d)",
                       t->name, (int)t->pid, t->miss, MISS_LIMIT);
            if (t->miss >= MISS_LIMIT) {
                aether_log(AETHER_LOG_CRITICAL,
                           "watchdog: killing %s (pid=%d) — IPC unresponsive for %d cycles; init will restart",
                           t->name, (int)t->pid, t->miss);
                if (kill(t->pid, SIGKILL) == 0) {
                    aether_log(AETHER_LOG_INFO, "watchdog: SIGKILL sent to %s (pid=%d)",
                               t->name, (int)t->pid);
                } else {
                    aether_log(AETHER_LOG_WARN, "watchdog: kill %s (pid=%d) failed (%s)",
                               t->name, (int)t->pid, strerror(errno));
                }
                t->miss = 0;   /* 重启由 init 负责；等新 pid 出现再监控 */
            }
        }
        for (int s = 0; s < PERIOD_S && !g_stop; s++) sleep(1);
    }

    if (g_wdt_fd >= 0) {
        ssize_t w = write(g_wdt_fd, "V", 1);   /* 魔术关狗：别让硬件狗误重启 */
        (void)w;
        close(g_wdt_fd);
    }
    aether_log(AETHER_LOG_INFO, "watchdog: stopped");
    return 0;
}
