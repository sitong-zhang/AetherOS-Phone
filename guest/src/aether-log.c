/*
 * aether-log.c - AetherOS PC 共享日志实现
 *
 * 仅依赖 libc，使用单调时钟计算相对启动秒数，避免时区问题。
 * 所有输出走 fd=2（stderr），由 PID1 在 boot 早期把 fd=2 重定向到
 * /dev/kmsg 与串口，因此守护进程无需关心 sink。
 */
#include "aether-log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *g_ident = "aether";
static double g_start_sec = 0.0;
static int g_inited = 0;

static double now_sec(void);

static void ensure_start(void)
{
    if (!g_inited) {
        g_start_sec = now_sec();
        g_inited = 1;
    }
}

void aether_log_ident(const char *ident)
{
    if (ident)
        g_ident = ident;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void emit(const char *tag, const char *fmt, va_list ap)
{
    ensure_start();
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;
    /* 前缀：[ident][TAG][+12.34s] message */
    char line[1200];
    int m = snprintf(line, sizeof line, "[%s][%s][+%.2fs] %.*s\n",
                     g_ident, tag ? tag : "?", now_sec() - g_start_sec, n, buf);
    if (m < 0)
        m = 0;
    (void)!write(2, line, (size_t)(m < 0 ? 0 : m));
}

void aether_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(tag, fmt, ap);
    va_end(ap);
}

void aether_log_fatal(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(tag, fmt, ap);
    va_end(ap);
    /* 给一点时间让日志刷出，再退出 */
    fsync(2);
    _exit(1);
}
