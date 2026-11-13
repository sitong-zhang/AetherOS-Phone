/*
 * AetherOS Kernel · 内核公共接口（freestanding）
 * 分层：L0 引导 → L1 CPU/中断 → L2 内存 → L3 驱动 → L4 进程/系统调用 → L5 安全服务
 * 所有可调参数来自 aether_kconfig.h（单一源）。
 */
#ifndef AETHER_KERNEL_H
#define AETHER_KERNEL_H
#include "aether_types.h"
#include "aether_kconfig.h"
#include "aether_multiboot.h"

/* ---- freestanding C 运行库（klib.c）---- */
void *kmemset(void *d, int c, size_t n);
void *kmemcpy(void *d, const void *s, size_t n);
void *kmemmove(void *d, const void *s, size_t n);
int   kmemcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);
int   kstrcmp(const char *a, const char *b);
int   kstrncmp(const char *a, const char *b, size_t n);
char *kstrncpy(char *d, const char *s, size_t n);
int   ksnprintf(char *buf, size_t n, const char *fmt, ...);
int   kvsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap);
void  khex(char *out, const uint8_t *data, size_t n);   /* out 需 2n+1 */
int   katoi(const char *s);

/* ---- 控制台（console.c）：串口 + VGA + 输入 ---- */
void con_init(void);
void con_putc(char c);                 /* 同时写串口与 VGA */
int  con_write(const char *s);
int  con_printf(const char *fmt, ...);
void con_clear(uint8_t attr);
void con_setattr(uint8_t attr);
void con_move(uint8_t col, uint8_t row);
void con_draw_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t attr, char border);
void con_puts_at(uint8_t x, uint8_t y, const char *s, uint8_t attr);
int  con_poll_char(char *out);         /* 非阻塞，返回 1 有字符 */
int  con_read_line(char *buf, size_t n, bool echo);  /* 阻塞读一行，返回长度 */
void con_irq_serial(char c);           /* 串口 RX 中断回调 */
void con_irq_keyboard(char c);         /* PS/2 键盘回调 */

/* ---- CPU / 中断（interrupts.c + isr.S）---- */
typedef struct K_PACKED {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, errcode;
    uint64_t rip, cs, rflags, rsp, ss;   /* CPU 自动压栈帧（跨 CPL 时有效） */
} regs_t;

void cpu_init(void);                  /* GDT/TSS/IDT/PIC 全量初始化 */
void idt_set(uint8_t vec, void (*handler)(void), uint8_t dpl);
void pic_eoi(uint8_t irq);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);
void isr_c_handler(regs_t *r);
extern volatile uint64_t g_ticks;
extern volatile uint32_t g_fault_count;
extern uint64_t g_kernel_pml4_phys;

/* ---- 内存（memory.c）---- */
void pmm_init(mb_info_t *mbi);
void *pmm_alloc(void);                /* 返回一个 4 KiB 物理页（已清零）*/
void  pmm_free(void *p);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
void vmm_init(void);
int  vmm_map(uint64_t vaddr, uint64_t paddr, uint64_t flags);
uint64_t vmm_pte_flags(uint64_t vaddr);  /* 诊断：返回页表项低12位（P/RW/US） */
int  vmm_map_user_range(uint64_t vaddr, uint64_t bytes); /* 拆 2MiB 大页为 4KiB 并置 U/S */
void *kheap_alloc(size_t n);
void  kheap_free(void *p);
uint64_t vmm_get_kernel_cr3(void);

/* ---- 设备（devices.c）---- */
void timer_init(void);                /* PIT 100 Hz */
void keyboard_init(void);             /* PS/2 IRQ1 */
void ata_init(void);                  /* ATA PIO LBA28 */
int  ata_read(uint32_t lba, void *buf);
int  ata_write(uint32_t lba, const void *buf);
int  ata_present(void);
uint32_t ata_rng_u32(void);           /* CSPRNG：RDRAND + 时钟白化 */

/* ---- 密码学（crypto.c，freestanding，复用 secure/ 的已验证算法思路）---- */
typedef struct { uint32_t h[8]; uint64_t total; uint8_t buf[64]; size_t len; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *p, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[K_HASH_BYTES]);
void sha256_buf(const void *p, size_t n, uint8_t out[K_HASH_BYTES]);
void hmac_sha256(const uint8_t *key, size_t kn, const void *msg, size_t mn,
                 uint8_t out[K_HASH_BYTES]);
void pbkdf2(const void *pw, size_t pn, const uint8_t *salt, size_t sn,
            uint32_t iter, uint8_t out[K_HASH_BYTES]);
void chacha20(const uint8_t key[K_KEY_BYTES], const uint8_t nonce[K_NONCE_BYTES],
              uint32_t counter, const void *in, void *out, size_t n);
int  ct_eq(const void *a, const void *b, size_t n);
void csprng(uint8_t *out, size_t n);

/* ---- 安全子系统（vault.c）：内核态服务 ---- */
#define KSEC_STATE_FACTORY  0
#define KSEC_STATE_LOCKED   1
#define KSEC_STATE_UNLOCKED 2
#define KSEC_STATE_WIPED    3

void ksec_init(void);                 /* 开机自检 + 从磁盘超块恢复状态 */
int  ksec_boot(char *msg, size_t n);  /* 完整性自检，msg 输出 */
int  ksec_setup(const char *pw, const char *duress);  /* 出厂初始化 */
int  ksec_unlock(const char *pw, char *msg, size_t n);/* 返回 K_AUTH_* */
int  ksec_lock(void);
int  ksec_put(const char *name, const void *data, size_t n);
int  ksec_get(const char *name, void *buf, size_t n, size_t *out_len);
int  ksec_list(char *buf, size_t n);
uint32_t ksec_fails(void);
int  ksec_state(void);
int  ksec_audit_dump(char *buf, size_t n);
int  ksec_audit_verify(void);
int  ksec_wipe_status(char *buf, size_t n);
int  ksec_selftest(char *buf, size_t n);
void ksec_emergency_wipe(const char *reason);
const char *ksec_last_receipt(void);
uint64_t ksec_sys_digest(void);       /* 系统区（内核代码）摘要，擦除前后比对 */
void ksec_idle_check(void);

/* ---- 进程 / ELF / 系统调用 ---- */
int  elf_load_and_run(const void *image, size_t len, char *msg, size_t n);
void syscall_init(void);
int  syscall_dispatch(regs_t *r);
void task_enter_user(uint64_t entry, uint64_t user_rsp);  /* isr.S，iretq 切 ring3 */

/* ---- 内核主入口 / 用户态退出收尾 ---- */
void K_NORETURN kmain(uint32_t magic, uint32_t mbi_phys);
void K_NORETURN kernel_finalize(int code);   /* SYS_EXIT 落点：打印总结并停机，不返回 */

/* ---- 嵌入的用户态 init 镜像（build.sh 由 objcopy 生成）---- */
extern const uint8_t _binary_init_elf_start[];
extern const uint8_t _binary_init_elf_end[];

#endif /* AETHER_KERNEL_H */
