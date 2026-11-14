/*
 * AetherOS Kernel · 进程表 + 抢占式调度器
 *
 * 设计（诚实边界）：当前为「共享内核页表」的多任务——两个 ring3 任务运行在同一套
 * 4 级页表上，各有独立的代码段与用户栈，由 PIT(100Hz) 时钟驱动做真实上下文切换与轮转。
 * 任务之间不通过 MMU 做地址空间隔离（隔离为后续阶段）；内核区(US=0)任务态不可访问。
 *
 * 切换机制：时钟中断里，isr_c_handler 已把完整上下文（通用寄存器 + iret 帧）压进当前
 * 内核栈帧，regs_t 指向上述帧。调度器把整帧复制进/出 PCB，并改写返回帧的 iret 字段，
 * isr_c_handler 返回后 iretq 即切到下一任务——这是厂商式「改写返回帧」上下文切换。
 */
#include "aether_kernel.h"

static proc_t g_proc[K_MAX_PROC];
proc_t *g_current = NULL;

void proc_init(void) {
    kmemset(g_proc, 0, sizeof g_proc);
    g_current = NULL;
}

proc_t *proc_create(const void *image, size_t len, uint64_t load_vaddr,
                    uint64_t stack_top, uint64_t stack_bytes,
                    const char *name, char *msg, size_t n) {
    int slot = -1;
    for (int i = 0; i < K_MAX_PROC; i++) {
        if (g_proc[i].pid == 0) { slot = i; break; }
    }
    if (slot < 0) { ksnprintf(msg, n, "PROC FAIL: no free slot\n"); return NULL; }
    uint64_t entry = 0, stack = 0;
    int rc = elf_load(image, len, load_vaddr, stack_top, stack_bytes, msg, n, &entry, &stack);
    if (rc) return NULL;
    proc_t *p = &g_proc[slot];
    kmemset(&p->ctx, 0, sizeof p->ctx);
    p->pid     = slot + 1;
    p->state   = 1;            /* READY */
    p->quantum = K_SCHED_QUANTUM;
    p->name    = name;
    p->ctx.rip    = entry;     /* 进入 ring3 的入口 */
    p->ctx.cs     = 0x1B;      /* 用户代码段 0x18 | RPL3 */
    p->ctx.rflags = 0x202;     /* IF | 保留位 */
    p->ctx.rsp    = stack;     /* 用户栈顶（已放 argc=0 帧） */
    p->ctx.ss     = 0x23;      /* 用户数据段 0x20 | RPL3 */
    return p;
}

/* 轮转挑选下一个 READY 任务，改写返回帧 r 使其 iretq 到该任务；无则原样返回当前。 */
static void sched_pick(regs_t *r) {
    proc_t *next = NULL;
    int start = g_current ? (int)(g_current - g_proc) : -1;
    for (int k = 1; k <= K_MAX_PROC; k++) {
        int i = (start + k) % K_MAX_PROC;
        if (g_proc[i].state == 1) { next = &g_proc[i]; break; }
    }
    if (!next) {
        if (g_current) kmemcpy(r, &g_current->ctx, sizeof(regs_t));
        return;
    }
    g_current = next;
    kmemcpy(r, &next->ctx, sizeof(regs_t));   /* 改写返回帧：iretq 切到 next */
}

/* PIT 中断(r=0x20)里调用：仅当用户态(CS.RPL=3)才抢占；保存当前整帧后轮转。 */
void sched_tick(regs_t *r) {
    if (!g_current) return;
    if ((r->cs & 3) != 3) return;            /* 只抢占 ring3，不碰 ring0 */
    kmemcpy(&g_current->ctx, r, sizeof(regs_t));   /* 保存当前（GP+iret） */
    if (g_current->quantum > 1) {            /* 时间片未到：原地续跑 */
        g_current->quantum--;
        kmemcpy(r, &g_current->ctx, sizeof(regs_t));
        return;
    }
    g_current->quantum = K_SCHED_QUANTUM;
    sched_pick(r);
}

/* 非 init 任务的系统调用 K_SYS_EXIT：标记退出并切换到其它任务（不终止整内核）。 */
void sched_exit_task(regs_t *r) {
    if (!g_current || g_current->pid == 1) {
        kernel_finalize((int)r->rdi);        /* init 退出 = 整内核收尾 */
        return;
    }
    g_current->state = 2;                     /* EXITED */
    g_current = NULL;
    sched_pick(r);
}

int proc_count_ready(void) {
    int c = 0;
    for (int i = 0; i < K_MAX_PROC; i++) if (g_proc[i].state == 1) c++;
    return c;
}

/* 设置 g_current=init 并由 init 首次进入 ring3（后续切换交给调度器）。 */
void proc_start_first(void) {
    if (!g_current) return;
    g_current->quantum = K_SCHED_QUANTUM;
    k_sti();
    task_enter_user(g_current->ctx.rip, g_current->ctx.rsp);
}
