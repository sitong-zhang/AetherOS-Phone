/*
 * aether-log.c - AetherOS 序列化日志客户端库实现（P2-13a）。
 *
 * 设计要点：
 *   - 单条记录 = 单个 datagram，发完即走（日志路径永不阻塞业务逻辑）；
 *   - /proc/uptime 真实读数填 monotonic 时间戳，不可读填 -1；
 *   - sendto 失败 -> /dev/kmsg 降级（printk 级别映射），失败也记不住不丢；
 *   - 重试节流：降级期间每 64 条尝试重连一次（logd 启动晚于调用方时
 *     能自动恢复主路径，无需调用方关心）。
 */
#include "aether-log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_SOCK_PATH   "/run/aether-log.sock"
#define KMSG_PATH       "/dev/kmsg"
#define RETRY_INTERVAL  64          /* 降级期间每 N 条重试一次 socket */

static char g_svc[64] = "unknown";
static int  g_fail_streak = 0;      /* 连续 sendto 失败计数 */
static int  g_msgs = 0;             /* 累计记录数（含降级） */

const char *aether_log_level_name(int level)
{
    switch (level) {
    case AETHER_LOG_DEBUG:    return "DEBUG";
    case AETHER_LOG_INFO:     return "INFO";
    case AETHER_LOG_WARN:     return "WARN";
    case AETHER_LOG_ERROR:    return "ERROR";
    case AETHER_LOG_CRITICAL: return "CRITICAL";
    default:                  return "?";
    }
}

int aether_log_using_daemon(void)
{
    return g_fail_streak == 0;
}

void aether_log_init(const char *svcname)
{
    if (!svcname || !*svcname) return;
    strncpy(g_svc, svcname, sizeof g_svc - 1);
    g_svc[sizeof g_svc - 1] = '\0';
}

/* /proc/uptime 单调时钟（秒，浮点）；不可读返回 -1，绝不编造。 */
static double proc_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1.0;
    double up = -1.0;
    if (fscanf(f, "%lf", &up) != 1) up = -1.0;
    fclose(f);
    return up;
}

/* kmsg 降级：printk 级别映射（KERN_INFO=6 / WARNING=4 / ERR=3 / CRIT=2）。 */
static int kmsg_level(int level)
{
    switch (level) {
    case AETHER_LOG_DEBUG:    return 6;
    case AETHER_LOG_INFO:     return 6;
    case AETHER_LOG_WARN:     return 4;
    case AETHER_LOG_ERROR:    return 3;
    case AETHER_LOG_CRITICAL: return 2;
    default:                  return 6;
    }
}

static void write_kmsg(int level, const char *msg)
{
    int fd = open(KMSG_PATH, O_WRONLY);
    if (fd < 0) return;                 /* 容器/无权限环境：静默跳过 */
    char line[1200];
    int n = snprintf(line, sizeof line, "<%d>aetheros-log: %s %s[%d]: %s\n",
                     kmsg_level(level), aether_log_level_name(level),
                     g_svc, (int)getpid(), msg);
    if (n > 0) {
        ssize_t w = write(fd, line, (size_t)n > sizeof line - 1 ? sizeof line - 1 : (size_t)n);
        (void)w;
    }
    close(fd);
}

void aether_log(int level, const char *fmt, ...)
{
    char msg[1024];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    double up = proc_uptime();

    /* 主路径：datagram 发给 aether-logd。降级期间按间隔重试。 */
    if (g_fail_streak == 0 || (++g_msgs % RETRY_INTERVAL) == 0) {
        char rec[1200];
        int n = snprintf(rec, sizeof rec, "1 %.2f %s %s %d %s",
                         up, aether_log_level_name(level), g_svc,
                         (int)getpid(), msg);
        if (n > 0 && n < (int)sizeof rec) {
            int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (fd >= 0) {
                struct sockaddr_un sa;
                memset(&sa, 0, sizeof sa);
                sa.sun_family = AF_UNIX;
                strncpy(sa.sun_path, LOG_SOCK_PATH, sizeof sa.sun_path - 1);
                ssize_t sent = sendto(fd, rec, (size_t)n, 0,
                                      (struct sockaddr *)&sa, sizeof sa);
                close(fd);
                if (sent == (ssize_t)n) {
                    g_fail_streak = 0;
                    return;
                }
            }
        }
        if (g_fail_streak == 0)
            g_fail_streak = 1;          /* 首次失败：进入降级 */
        else
            g_fail_streak++;            /* 重试仍失败：继续降级 */
    }

    /* 降级路径：直写内核日志环（/run/aether-init.log 等本地文件由调用方
     * 自行保留——本库不替代调用方已有的落盘行为）。 */
    write_kmsg(level, msg);
}
