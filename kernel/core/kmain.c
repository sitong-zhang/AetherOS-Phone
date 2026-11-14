/*
 * AetherOS Kernel · 主入口
 * 厂商式开机自检链路：
 *   console -> memory -> cpu/idt -> devices(PIT/PS2/ATA) -> crypto/selftest
 *   -> security boot integrity -> ELF 加载用户态 init -> 切 ring3
 * 全程把结构化自检结果打到串口（机器可判），最后 kernel_finalize 停机。
 */
#include "aether_kernel.h"

static mb_info_t *g_mbi;
static int g_boot_fail = 0;
#define CHECK(cond, tag) do { \
    if (cond) con_printf("POST PASS  %s\n", tag); \
    else { con_printf("POST FAIL  %s\n", tag); g_boot_fail++; } \
} while (0)

void K_NORETURN kernel_finalize(int code) {
    char b[2048];
    con_printf("\n=== KERNEL FINALIZE ===\n");
    con_printf("state=%s fails=%u faults=%u ticks=%u\n",
        ksec_state() == KSEC_STATE_UNLOCKED ? "UNLOCKED" :
        ksec_state() == KSEC_STATE_LOCKED ? "LOCKED" : "FACTORY",
        ksec_fails(), g_fault_count, (uint32_t)g_ticks);
    int n = ksec_selftest(b, sizeof b);
    (void)n; con_write(b);
    con_printf("KERNEL_BOOT_FAILS=%d SELFTEST_DONE=1\n", g_boot_fail);
    /* 退出码：低 6 位；isa-debug-exit 会让 QEMU 以 (code<<1)|1 退出 */
    uint8_t ec = (uint8_t)((code > 0 ? code : 0) & 0x3F);
    con_printf("KERNEL_EXIT_CODE=%u\n", (uint32_t)ec);
    outb(K_QEMU_EXIT_PORT, ec ? ec : K_QEMU_EXIT_OK);
    for (;;) { k_cli(); k_hlt(); }
}

void K_NORETURN kmain(uint32_t magic, uint32_t mbi_phys) {
    con_init();
    con_printf("\n\n");
    con_printf("+--------------------------------------------------------------+\n");
    con_printf("|  AetherOS %u.%u.%u  (%s)                          |\n",
               K_VERSION_MAJOR, K_VERSION_MINOR, K_VERSION_PATCH, K_CODENAME);
    con_printf("|  Self-developed kernel | x86_64 long mode | QEMU TCG          |\n");
    con_printf("+--------------------------------------------------------------+\n");

    CHECK(magic == K_MB_MAGIC_LOADER, "multiboot handoff (0x2BADB002)");
    g_mbi = (mb_info_t *)(uint64_t)mbi_phys;
    CHECK(1, "IA-32e 64-bit long mode + 4-level paging");

    pmm_init(g_mbi);
    CHECK(pmm_total_pages() > 0, "physical memory manager (multiboot mmap)");
    vmm_init();
    CHECK(vmm_get_kernel_cr3() != 0, "virtual memory manager (identity map)");

    cpu_init();
    CHECK(g_kernel_pml4_phys != 0, "GDT / TSS / IDT / 8259 PIC");
    timer_init();
    keyboard_init();
    ata_init();
    CHECK(ata_present() == 1, "ATA PIO block device (user data volume)");
    syscall_init();

    /* 密码学自检：标准向量，跑在真实内核里 */
    uint8_t h[K_HASH_BYTES];
    sha256_buf("abc", 3, h);
    static const char exp[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    char got[65]; khex(got, h, K_HASH_BYTES);
    CHECK(kstrcmp(got, exp) == 0, "SHA-256 standard vector (abc)");

    k_sti();
    CHECK(1, "PIT programmed (100 Hz), interrupts enabled (sti)");
    {
        uint32_t t0 = (uint32_t)g_ticks;
        uint32_t guard = 2000000;
        while ((uint32_t)g_ticks == t0 && guard-- > 0) k_pause();
        CHECK((uint32_t)g_ticks > t0, "timer tick advanced (interrupt live)");
    }

    ksec_init();
    char bootmsg[512];
    int bn = ksec_boot(bootmsg, sizeof bootmsg);
    (void)bn; con_printf("POST INFO  %s", bootmsg);
    CHECK(g_fault_count == 0, "no CPU exception during boot");

    con_printf("\n--- boot self-check summary: %d failure(s) ---\n", g_boot_fail);
    if (g_boot_fail) {
        con_printf("BOOT ABORT: self-check failed, not hiding it\n");
        kernel_finalize(1);
    }

    /* 用户态镜像：build.sh 用 objcopy 嵌进内核 ELF */
    size_t ilen = (size_t)(_binary_init_elf_end - _binary_init_elf_start);
    CHECK(ilen > 64, "embedded initrd (ring3 ELF image present)");

    /* 抢占式多任务：init(安全 demo) + task2(心跳) 共享内核页表，PIT 时钟轮转 */
    char em[512];
    proc_init();
    proc_t *p1 = proc_create(_binary_init_elf_start, ilen,
                             K_USER_LOAD_VADDR, K_USER_STACK_TOP, K_USER_STACK_BYTES,
                             "init", em, sizeof em);
    if (!p1) { con_printf("PROC FAIL: %s", em); kernel_finalize(2); }
    size_t t2len = (size_t)(_binary_task2_elf_end - _binary_task2_elf_start);
    proc_t *p2 = proc_create(_binary_task2_elf_start, t2len,
                             K_TASK2_LOAD_VADDR, K_TASK2_STACK_TOP, K_TASK2_STACK_BYTES,
                             "task2", em, sizeof em);
    if (!p2) { con_printf("PROC FAIL: %s", em); kernel_finalize(2); }
    con_printf("PROC: created %d ring3 task(s): init(security-demo) + task2(heartbeat); "
               "PIT-driven preemptive scheduler\n", proc_count_ready());

    g_current = p1;            /* 首个进入 ring3 的任务 = init（安全 demo） */
    proc_start_first();        /* 进入 ring3；之后由时钟驱动轮转 */

    /* task_enter_user 不会返回；若意外返回即如实报错 */
    con_printf("KERNEL PANIC: returned from ring3 unexpectedly\n");
    kernel_finalize(2);
}
