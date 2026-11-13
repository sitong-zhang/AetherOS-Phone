/*
 * AetherOS Kernel · 系统调用分发（int 0x80，DPL=3）
 * 约定：rax=调用号，rdi/rsi/rdx=参数 1/2/3；返回值写回 regs->rax。
 * 用户态缓冲区地址需已在页表中置 U/S（elf 加载时映射），内核以 CPL0 访问。
 */
#include "aether_kernel.h"

void syscall_init(void) { /* IDT 已在 cpu_init 中把 0x80 设为 DPL3 */ }

int syscall_dispatch(regs_t *r) {
    uint32_t nr = (uint32_t)r->rax;
    const char *a1 = (const char *)(uintptr_t)r->rdi;
    void       *a2 = (void *)(uintptr_t)r->rsi;
    uint32_t    a3 = (uint32_t)r->rdx;
    char tmp[4096];

    switch (nr) {
    case K_SYS_WRITE: {
        /* u_write(s, n)：usyscall 把 s->rdi(a1)、n->rsi(a2)；长度必须从 a2 取，
         * 之前错取 a3(=rdx=0) 导致 kmemcpy 拷 0 字节 -> 所有用户输出为空串。 */
        size_t wn = (size_t)(uintptr_t)a2;
        if (!a1 || wn > sizeof tmp) return -1;
        kmemcpy(tmp, a1, wn);
        tmp[wn < sizeof tmp ? wn : sizeof tmp - 1] = 0;
        return con_printf("%s", tmp);
    }
    case K_SYS_READ: {
        if (!a1 || !a2) return 0;
        /* 非阻塞读字符（串口/键盘输入队列）；lim 取 a2（第二个用户参数） */
        char c;
        char *out = (char *)(uintptr_t)r->rdi;
        uint32_t lim = (uint32_t)(uintptr_t)r->rsi;
        uint32_t n = 0;
        while (n + 1 < lim && con_poll_char(&c)) { out[n++] = c; }
        out[n] = 0;
        return (int)n;
    }
    case K_SYS_GET_TICKS:
        return (int)(uint32_t)g_ticks;
    case K_SYS_SYSINFO: {
        char *o = (char *)(uintptr_t)r->rdi;
        uint32_t lim = (uint32_t)r->rsi;
        return ksnprintf(o, lim,
            "AetherOS %u.%u.%u (%s) | kernel=ring0 | pages=%u/%u | ticks=%u | faults=%u\n",
            K_VERSION_MAJOR, K_VERSION_MINOR, K_VERSION_PATCH, K_CODENAME,
            (uint32_t)pmm_free_pages(), (uint32_t)pmm_total_pages(),
            (uint32_t)g_ticks, g_fault_count);
    }
    case K_SYS_LOGIN:
        /* 出厂初始化：a1=口令, a2=胁迫口令(可空) */
        if (!a1) return -1;
        return ksec_setup(a1, a2 ? (const char *)a2 : NULL);
    case K_SYS_UNLOCK: {
        if (!a1) return K_AUTH_LOCKED;
        char *o = (char *)(uintptr_t)r->rsi;
        uint32_t lim = (uint32_t)r->rdx;
        return ksec_unlock(a1, lim ? o : tmp, lim ? lim : sizeof tmp);
    }
    case K_SYS_LOCK:
        return ksec_lock();
    case K_SYS_VAULT_PUT: {
        if (!a1 || !a2) return -1;
        /* u_put(name, data, len)：usyscall 把 name->rdi(a1)、data->rsi(a2)、
         * len->rdx(a3)；之前错把 data 取 rdx、len 取 rcx，导致写入错位。 */
        const void *data = (const void *)(uintptr_t)r->rsi;
        uint32_t len = (uint32_t)r->rdx;
        (void)a3;
        return ksec_put(a1, data, len);
    }
    case K_SYS_VAULT_GET: {
        if (!a1 || !a2) return -1;
        size_t got = 0;
        return ksec_get(a1, a2, (uint32_t)r->rdx, &got);
    }
    case K_SYS_VAULT_LIST:
        return ksec_list((char *)(uintptr_t)r->rdi, (uint32_t)r->rsi);
    case K_SYS_WIPE_STATUS:
        return ksec_wipe_status((char *)(uintptr_t)r->rdi, (uint32_t)r->rsi);
    case K_SYS_AUDIT_DUMP:
        return ksec_audit_dump((char *)(uintptr_t)r->rdi, (uint32_t)r->rsi);
    case K_SYS_SELFTEST:
        return ksec_selftest((char *)(uintptr_t)r->rdi, (uint32_t)r->rsi);
    case K_SYS_BOOT_MAGIC:
        return (int)K_BUILD_MAGIC;
    case K_SYS_EXIT:
        /* 用户态主动退出：不能 return（iretq 会回用户态），
         * 直接跳进内核收尾：打印最终状态、可选截图点、QEMU 退出。 */
        kernel_finalize((int)r->rdi);
    default:
        return -255;
    }
}
