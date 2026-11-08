/*
 * aether-watchdog.c - AetherOS PC 健康看门狗
 *
 * 职责：对关键服务做心跳探测，发现持续无响应（卡死）的进程并发信号，
 * 交由 init（restart=always）自动重启。
 *
 * 关键正确性约束（避免误杀）：
 *   - 套接字尚不存在 / 进程未就绪属于“启动期”，不计为失活；
 *   - 仅当连续 MISS_LIMIT 次（默认 3）心跳全部超时，才判定为卡死并 kill；
 *   - 杀掉的是具体 pid，不碰其他服务。
 *
 * 这是 PC 原生实现，与手机基线无共享代码。
 */
#include "aether-log.h"

#include <arpa/inet.h>
#include <ctype.h>
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

#define COMP_SOCK "/run/aetheros-comp.sock"
#define SYS_SOCK "/run/aetheros.sock"
#define MISS_LIMIT 3
#define HB_TIMEOUT_MS 2500
#define PERIOD_SEC 5

static volatile int g_stop = 0;
static void on_signal(int s)
{
    (void)s;
    g_stop = 1;
}

/* 返回指定服务当前 pid（从 init 维护的 tsv 读取），0 表示未发现 */
static pid_t read_pid(const char *name)
{
    FILE *f = fopen("/run/aether/services.tsv", "r");
    if (!f)
        return 0;
    char line[256];
    pid_t pid = 0;
    while (fgets(line, sizeof line, f)) {
        char svc[64];
        int p;
        char state[16];
        if (sscanf(line, "%63s\t%d\t%15s", svc, &p, state) == 3) {
            if (strcmp(svc, name) == 0) {
                pid = (pid_t)p;
                break;
            }
        }
    }
    fclose(f);
    return pid;
}

/* 对 UDS 做 ping 心跳：连接 -> 发 "ping" -> 等 "pong"。返回 0 表示存活 */
static int heartbeat(const char *sock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sock, sizeof a.sun_path - 1);
    /* 非阻塞连接，便于在启动期快速失败而不卡住 */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int cr = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (cr < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1; /* 套接字未就绪（启动期） */
    }
    struct pollfd pfd = {fd, POLLOUT, 0};
    if (poll(&pfd, 1, HB_TIMEOUT_MS) <= 0) {
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fl);
    const char *ping = "ping\n";
    if (send(fd, ping, strlen(ping), 0) < 0) {
        close(fd);
        return -1;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, HB_TIMEOUT_MS) <= 0) {
        close(fd);
        return -1;
    }
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    return strstr(buf, "pong") ? 0 : -1;
}

static int watch_one(const char *svc, const char *sock, int *miss)
{
    pid_t pid = read_pid(svc);
    if (pid <= 0)
        return 0; /* 服务未就绪，跳过 */
    int ok = heartbeat(sock);
    if (ok == 0) {
        if (*miss != 0) {
            *miss = 0;
            aether_log("watchdog", "%s 心跳恢复", svc);
        }
        return 0;
    }
    /* 失活：先判断是否套接字根本没起来（启动期），此时不计 */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sock, sizeof a.sun_path - 1);
    int probe = connect(fd, (struct sockaddr *)&a, sizeof a);
    close(fd);
    if (probe < 0 && (errno == ENOENT || errno == ECONNREFUSED)) {
        /* 套接字还没建立，启动期，不计失活 */
        return 0;
    }
    (*miss)++;
    if (*miss >= MISS_LIMIT) {
        aether_log("watchdog", "CRITICAL: %s 连续 %d 次心跳无响应，发送 SIGKILL (pid=%d)",
                   svc, *miss, (int)pid);
        kill(pid, SIGKILL); /* 由 init 自动重启 */
        *miss = 0;
    } else {
        aether_log("watchdog", "%s 心跳超时 (miss=%d/%d)", svc, *miss, MISS_LIMIT);
    }
    return 0;
}

int main(void)
{
    aether_log_ident("watchdog");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    aether_log("watchdog", "看门狗启动 (心跳周期 %ds, 失活上限 %d)", PERIOD_SEC, MISS_LIMIT);

    int miss_comp = 0, miss_sys = 0;
    while (!g_stop) {
        watch_one("aether-compositor", COMP_SOCK, &miss_comp);
        watch_one("aetherosd", SYS_SOCK, &miss_sys);
        /* 周期睡眠（可被信号中断） */
        for (int i = 0; i < PERIOD_SEC && !g_stop; i++)
            sleep(1);
    }
    aether_log("watchdog", "退出");
    return 0;
}
