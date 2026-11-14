/*
 * AetherOS · 第二 ring3 任务（演示抢占式多任务）
 *
 * 与 init(安全 demo) 共享内核页表、各有独立代码段与栈。本任务只做心跳：
 * 周期性打印自己的 pid / 当前 tick / 心跳序号，证明调度器在 init 与 task2 之间真实轮转。
 * 不触碰磁盘 / 安全子系统，不与 init 的演示冲突。
 */
#include "aether_uapi.h"

static char g_buf[256];

static void cat(char **p, const char *s) { while (*s) *(*p)++ = *s++; }
static void dec(char **p, long v) {
    char t[16]; uintoa(t, (unsigned long)(v < 0 ? -v : v));
    if (v < 0) *(*p)++ = '-';
    int i = 0; while (t[i]) *(*p)++ = t[i++];
}

int umain(void) {
    long pid   = u_getpid();
    long start = u_get_ticks();
    long last  = start;
    long beats = 0;
    for (;;) {
        long t = u_get_ticks();
        if (t - last >= 8) {                 /* 每 ~80ms 心跳一次（100Hz） */
            last = t;
            char *p = g_buf;
            cat(&p, "TASK2 PID=");  dec(&p, pid);
            cat(&p, " tick=");     dec(&p, t);
            cat(&p, " HEARTBEAT #"); dec(&p, beats);
            *p++ = '\n'; *p = 0;
            u_write(g_buf);
            beats++;
        }
        u_yield();                            /* 主动让出（切换由时钟驱动） */
        if (t - start > 40) {                 /* 跑约 0.4s（~40 tick）后退出，赶在测试窗口收尾前完成 */
            char *p = g_buf;
            cat(&p, "TASK2 DONE pid="); dec(&p, pid);
            *p++ = '\n'; *p = 0;
            u_write(g_buf);
            usyscall(K_SYS_EXIT, 0, 0, 0, 0, 0);   /* 标记 EXITED 并切换，不终止内核 */
        }
    }
}
