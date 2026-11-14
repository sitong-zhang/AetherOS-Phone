/*
 * aether-init.c - AetherOS 的 PID 1 服务管理器
 *
 * 这是电脑操作系统的第一个用户态进程，职责：
 *   1. 挂载基础文件系统（proc/sys/dev/tmp/run）并设主机名
 *   2. 解析 /etc/aether/init.d/<svc> 声明式服务配置
 *   3. 按 depends 拓扑顺序启动服务
 *   4. 作为 PID1 回收所有子进程（僵尸），按 restart 策略重启
 *   5. 就绪后写入 /run/aether/ready 标记
 *
 * 设计原则（与手机基线彻底解耦）：
 *   - 不依赖任何 Android / HAL / aarch64 概念
 *   - 服务配置是极简 key=value，无 shell 求值
 *   - 阻塞式 waitpid(-1) 回收，天然避免 SIGCHLD 竞态
 */
#include "aether-log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_SVC 64
#define MAX_ARGS 32
#define INITD_DIR "/etc/aether/init.d"

enum restart_policy { RP_NEVER, RP_ONFAIL, RP_ALWAYS };

struct service {
    char name[64];
    char exec[MAX_ARGS][256];
    int nargs;
    char depends[8][64];
    int ndep;
    enum restart_policy rp;
    pid_t pid;
    int state; /* 0=down 1=up */
    int restarts;
};

static struct service svc[MAX_SVC];
static int nsvc = 0;

static void mount_one(const char *src, const char *target, const char *fstype)
{
    if (mkdir(target, 0755) != 0 && errno != EEXIST) { /* ignore */ }
    if (mount(src, target, fstype, 0, NULL) != 0) {
        /* 内核已自动挂载（CONFIG_DEVTMPFS_MOUNT=y 时 /dev 就是如此） */
        if (errno == EBUSY)
            return;
        aether_log("init", "mount %s (%s) 失败: %s", target, fstype, strerror(errno));
    }
}

static void early_setup(void)
{
    aether_log_ident("init");
    /* 控制台：保证 0/1/2 指向 /dev/console（即内核 console=ttyS0） */
    int c = open("/dev/console", O_RDWR);
    if (c >= 0) {
        dup2(c, 0);
        dup2(c, 1);
        dup2(c, 2);
        if (c > 2)
            close(c);
    }
    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/dev", 0755);
    mkdir("/tmp", 0777);
    mkdir("/run", 0755);
    mount_one("proc", "/proc", "proc");
    mount_one("sysfs", "/sys", "sysfs");
    mount_one("devtmpfs", "/dev", "devtmpfs");
    mount_one("tmpfs", "/tmp", "tmpfs");
    mount_one("tmpfs", "/run", "tmpfs");
    /* 在 tmpfs /run 挂载之后重建子目录，否则会被覆盖丢失 */
    mkdir("/run/aether", 0755);
    mkdir("/dev/pts", 0755);
    mount_one("devpts", "/dev/pts", "devpts");
    (void)sethostname("aetheros", 8);
}

/* 解析单个服务配置文件（极简 key=value） */
static int parse_service(const char *path, struct service *s)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    memset(s, 0, sizeof *s);
    s->rp = RP_ONFAIL;
    s->nargs = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] == '#' || line[0] == 0)
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "name") == 0) {
            strncpy(s->name, val, sizeof s->name - 1);
        } else if (strcmp(key, "exec") == 0) {
            /* 按空格切分 argv（不展开引号，足够本系统使用）。
             * 逐 token 先截断再复制，保证每个 argv 元素不含空格：
             * 否则 "cmd arg" 会被整体当作可执行文件路径，execv 必然失败。 */
            char *p = val;
            while (*p && s->nargs < MAX_ARGS) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                char *start = p;
                while (*p && *p != ' ')
                    p++;
                char save = *p;
                *p = 0;
                strncpy(s->exec[s->nargs], start, sizeof s->exec[s->nargs] - 1);
                s->exec[s->nargs][sizeof s->exec[s->nargs] - 1] = 0;
                s->nargs++;
                if (!save)
                    break;
                *p = save;
            }
        } else if (strcmp(key, "depends") == 0) {
            char *p = val;
            while (*p && s->ndep < 8) {
                while (*p == ' ')
                    p++;
                if (!*p)
                    break;
                char *start = p;
                while (*p && *p != ' ')
                    p++;
                char save = *p;
                *p = 0;
                strncpy(s->depends[s->ndep], start, sizeof s->depends[s->ndep] - 1);
                s->depends[s->ndep][sizeof s->depends[s->ndep] - 1] = 0;
                s->ndep++;
                if (!save)
                    break;
                *p = save;
            }
        } else if (strcmp(key, "restart") == 0) {
            if (strcmp(val, "always") == 0)
                s->rp = RP_ALWAYS;
            else if (strcmp(val, "never") == 0)
                s->rp = RP_NEVER;
            else
                s->rp = RP_ONFAIL;
        }
    }
    fclose(f);
    if (s->name[0] == 0 || s->nargs == 0) {
        aether_log("init", "跳过无效服务配置 %s", path);
        return -1;
    }
    return 0;
}

static void write_status(void)
{
    char path[256];
    snprintf(path, sizeof path, "%s/services.tsv", "/run/aether");
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < nsvc; i++)
        fprintf(f, "%s\t%d\t%s\n", svc[i].name, (int)svc[i].pid,
                svc[i].state ? "up" : "down");
    fclose(f);
}

static int dep_up(const struct service *s)
{
    for (int i = 0; i < s->ndep; i++) {
        int found = 0;
        for (int j = 0; j < nsvc; j++) {
            if (strcmp(svc[j].name, s->depends[i]) == 0) {
                if (!svc[j].state)
                    return 0;
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
    }
    return 1;
}

static void start_service(struct service *s)
{
    pid_t pid = fork();
    if (pid < 0) {
        aether_log("init", "fork %s 失败: %s", s->name, strerror(errno));
        return;
    }
    if (pid == 0) {
        /* 子进程：setsid 脱离，重定向 stdio 到控制台 */
        setsid();
        int c = open("/dev/console", O_RDWR);
        if (c >= 0) {
            dup2(c, 0);
            dup2(c, 1);
            dup2(c, 2);
            if (c > 2)
                close(c);
        }
        char *argv[MAX_ARGS + 1];
        for (int i = 0; i < s->nargs; i++)
            argv[i] = s->exec[i];
        argv[s->nargs] = NULL;
        execv(argv[0], argv);
        /* exec 失败 */
        fprintf(stderr, "[init] exec %s 失败: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    s->pid = pid;
    s->state = 1;
    aether_log("init", "启动服务 %s (pid=%d)", s->name, (int)pid);
    write_status();
}

static void start_all(void)
{
    /* 简单拓扑：反复扫描，直到没有可启动的新服务（最多 nsvc 轮） */
    int progress = 1;
    int rounds = 0;
    while (progress && rounds < nsvc + 2) {
        progress = 0;
        rounds++;
        for (int i = 0; i < nsvc; i++) {
            if (svc[i].state == 0 && dep_up(&svc[i])) {
                start_service(&svc[i]);
                progress = 1;
            }
        }
    }
    for (int i = 0; i < nsvc; i++) {
        if (svc[i].state == 0)
            aether_log("init", "警告：服务 %s 因依赖未满足而未启动", svc[i].name);
    }
}

static void handle_exit(pid_t pid, int status)
{
    for (int i = 0; i < nsvc; i++) {
        if (svc[i].pid == pid) {
            svc[i].state = 0;
            svc[i].pid = 0;
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            aether_log("init", "服务 %s 退出 (pid=%d code=%d)", svc[i].name, (int)pid, code);
            int should = (svc[i].rp == RP_ALWAYS) ||
                         (svc[i].rp == RP_ONFAIL && code != 0);
            if (should && svc[i].restarts < 20) {
                svc[i].restarts++;
                usleep(200000); /* 退避，避免紧密重启风暴 */
                start_service(&svc[i]);
            } else if (should) {
                aether_log("init", "服务 %s 重启次数过多，放弃", svc[i].name);
            }
            write_status();
            return;
        }
    }
}

int main(void)
{
    early_setup();
    aether_log("init", "AetherOS 启动 (PID 1)");

    /* 加载服务配置（使用 dirent 遍历） */
    {
        DIR *d = opendir(INITD_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && nsvc < MAX_SVC) {
                if (e->d_name[0] == '.')
                    continue;
                char p[300];
                snprintf(p, sizeof p, "%s/%s", INITD_DIR, e->d_name);
                struct service s;
                if (parse_service(p, &s) == 0)
                    svc[nsvc++] = s;
            }
            closedir(d);
        } else {
            aether_log("init", "无法打开 %s: %s", INITD_DIR, strerror(errno));
        }
    }

    aether_log("init", "加载了 %d 个服务", nsvc);
    start_all();

    /* 标记就绪 */
    int fd = open("/run/aether/ready", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        (void)write(fd, "1\n", 2);
        close(fd);
    }
    aether_log("init", "所有可启动服务已拉起，系统就绪");

    /* PID1 主循环：阻塞回收，按需重启 */
    while (1) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid > 0)
            handle_exit(pid, status);
        else if (pid < 0 && errno == ECHILD) {
            /* 没有子进程了（理论上不应发生）：短暂休眠避免空转 */
            usleep(500000);
        }
    }
    return 0;
}
