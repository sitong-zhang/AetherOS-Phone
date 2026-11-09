/*
 * aetherosd.c - AetherOS 系统守护进程
 *
 * 职责：
 *   - 通过 Unix 域套接字 /run/aetheros.sock 暴露 "AetherBus" 契约
 *   - 实时采集 CPU/内存/运行时长（读 /proc）
 *   - 汇总服务状态（读 init 维护的 /run/aether/services.tsv）
 *
 * 协议：行式文本，每条命令一行，回复一行（JSON-ish 文本）。
 *   ping              -> pong
 *   system            -> {"cpu":..,"mem":..,"uptime":..,"svc_up":..,"svc_total":..,"ver":".."}
 *   services          -> 每行 "name<TAB>state"
 *
 * 用户态守护实现，host 与客机共用同一套源码。
 */
#include "aether-log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCK_PATH "/run/aetheros.sock"
#define VERSION "pc-v1"

/* ---- /proc 采集 ---- */
static unsigned long g_prev[4] = {0, 0, 0, 0}; /* user,nice,system,idle(+iowait) */
static int g_have_prev = 0;

static double read_cpu_percent(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    char line[256];
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    unsigned long u, n, s, id, io = 0;
    /* cpu  user nice system idle iowait irq softirq ... */
    if (sscanf(line, "cpu %lu %lu %lu %lu %lu", &u, &n, &s, &id, &io) < 4)
        return -1;
    unsigned long idle = id + io;
    unsigned long total = u + n + s + idle;
    double pct = -1;
    if (g_have_prev) {
        unsigned long dt = total - (g_prev[0] + g_prev[1] + g_prev[2] + g_prev[3]);
        unsigned long di = idle - (g_prev[3]);
        if (dt > 0)
            pct = (double)(dt - di) / (double)dt * 100.0;
    }
    g_prev[0] = u;
    g_prev[1] = n;
    g_prev[2] = s;
    g_prev[3] = idle;
    g_have_prev = 1;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

static double read_mem_percent(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    unsigned long total = 0, avail = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemTotal: %lu", &total) == 1)
            continue;
        if (sscanf(line, "MemAvailable: %lu", &avail) == 1)
            continue;
    }
    fclose(f);
    if (total == 0)
        return -1;
    double used = (double)(total - avail) / (double)total * 100.0;
    if (used < 0)
        used = 0;
    if (used > 100)
        used = 100;
    return used;
}

static double read_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        return 0;
    double up = 0;
    fscanf(f, "%lf", &up);
    fclose(f);
    return up;
}

static void count_services(int *up, int *total)
{
    *up = 0;
    *total = 0;
    FILE *f = fopen("/run/aether/services.tsv", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        (*total)++;
        if (strstr(line, "up"))
            (*up)++;
    }
    fclose(f);
}

static void reply_system(char *out, size_t sz)
{
    double cpu = read_cpu_percent();
    double mem = read_mem_percent();
    double up = read_uptime();
    int su = 0, st = 0;
    count_services(&su, &st);
    snprintf(out, sz,
             "{\"cpu\":%.1f,\"mem\":%.1f,\"uptime\":%.1f,\"svc_up\":%d,\"svc_total\":%d,\"ver\":\"%s\"}",
             cpu, mem, up, su, st, VERSION);
}

static void reply_services(char *out, size_t sz)
{
    size_t off = 0;
    FILE *f = fopen("/run/aether/services.tsv", "r");
    if (!f) {
        snprintf(out, sz, "");
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        /* 去掉换行 */
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = 0;
        off += (size_t)snprintf(out + off, sz - off, "%s\n", line);
    }
    fclose(f);
}

static void handle_client(int fd)
{
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0)
        return;
    buf[n] = 0;
    /* 取第一行命令 */
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
    char cmd[64];
    sscanf(buf, "%63s", cmd);

    char reply[4096];
    if (strcmp(cmd, "ping") == 0) {
        snprintf(reply, sizeof reply, "pong\n");
    } else if (strcmp(cmd, "system") == 0) {
        reply_system(reply, sizeof reply);
        size_t L = strlen(reply);
        reply[L] = '\n';
        reply[L + 1] = 0;
    } else if (strcmp(cmd, "services") == 0) {
        reply_services(reply, sizeof reply);
    } else {
        snprintf(reply, sizeof reply, "unknown\n");
    }
    (void)send(fd, reply, strlen(reply), 0);
}

int main(void)
{
    aether_log_ident("aetherosd");
    aether_log("aetherosd", "系统守护启动 (AetherBus @ %s)", SOCK_PATH);

    unlink(SOCK_PATH);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        aether_log_fatal("aetherosd", "socket 失败: %s", strerror(errno));
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        aether_log_fatal("aetherosd", "bind %s 失败: %s", SOCK_PATH, strerror(errno));
        return 1;
    }
    listen(sfd, 8);
    chmod(SOCK_PATH, 0666);

    struct pollfd pfds[1];
    pfds[0].fd = sfd;
    pfds[0].events = POLLIN;
    while (1) {
        int pr = poll(pfds, 1, 1000);
        if (pr > 0 && (pfds[0].revents & POLLIN)) {
            int cfd = accept(sfd, NULL, NULL);
            if (cfd >= 0) {
                handle_client(cfd);
                close(cfd);
            }
        }
    }
    return 0;
}
