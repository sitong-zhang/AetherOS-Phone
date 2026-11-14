/*
 * AetherOS Kernel · CPU 描述符与中断控制器
 * GDT/TSS/IDT/8259 PIC；异常打印、IRQ 分发（PIT/PS2/COM1）；系统调用从这里进。
 */
#include "aether_kernel.h"

volatile uint64_t g_ticks = 0;
volatile uint32_t g_fault_count = 0;
uint64_t g_kernel_pml4_phys = 0;

extern void (*isr_stub_table[256])(void);
extern uint64_t gdt64[];
extern void isr_common(void);

/* ---- IDT ---- */
typedef struct K_PACKED {
    uint16_t offset_lo, selector;
    uint8_t  ist, attr;
    uint16_t offset_mid;
    uint32_t offset_hi, zero;
} idt_entry_t;
typedef struct K_PACKED { uint16_t limit; uint64_t base; } idtr_t;
static idt_entry_t g_idt[K_IDT_ENTRIES] K_ALIGN(16);

void idt_set(uint8_t vec, void (*handler)(void), uint8_t dpl) {
    uint64_t a = (uint64_t)(uintptr_t)handler;
    g_idt[vec].offset_lo  = (uint16_t)(a & 0xFFFF);
    g_idt[vec].selector   = 0x08;
    g_idt[vec].ist        = 0;
    g_idt[vec].attr       = (uint8_t)(0x80 | ((dpl & 3) << 5) | 0x0E); /* P|DPL|64-bit int gate */
    g_idt[vec].offset_mid = (uint16_t)((a >> 16) & 0xFFFF);
    g_idt[vec].offset_hi  = (uint32_t)((a >> 32) & 0xFFFFFFFFu);
    g_idt[vec].zero       = 0;
}
static void idt_load(void) {
    idtr_t r; r.limit = (uint16_t)(sizeof(g_idt) - 1); r.base = (uint64_t)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" :: "m"(r) : "memory");
}

/* ---- TSS（ring3 -> ring0 栈切换必需）---- */
typedef struct K_PACKED {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} tss_t;
static tss_t g_tss K_ALIGN(16);
static uint8_t g_syscall_stack[16384] K_ALIGN(16);

static void tss_setup(void) {
    kmemset(&g_tss, 0, sizeof g_tss);
    g_tss.rsp0 = (uint64_t)(uintptr_t)(g_syscall_stack + sizeof g_syscall_stack);
    g_tss.iomap_base = sizeof(tss_t);
    uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    uint32_t limit = sizeof(tss_t) - 1;
    /* x86_64 TSS 描述符占两个 GDT 槽（16 字节），type=0x9 (available 64-bit TSS) */
    uint64_t lo = ((uint64_t)(limit & 0xFFFF) << 0)
                | ((base & 0xFFFFFF) << 16)
                | (0x89ull << 40)
                | (((base >> 24) & 0xFF) << 56)
                | ((uint64_t)((limit >> 16) & 0xF) << 48);
    uint64_t hi = base >> 32;
    gdt64[5] = lo;
    gdt64[6] = hi;
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)0x28) : "memory");  /* TSS 在第 5 个 GDT 槽 = 0x28，无 RPL */
}

/* ---- 8259 PIC ---- */
static void pic_remap(void) {
    outb(K_PIC1_CMD, 0x11); io_wait(); outb(K_PIC2_CMD, 0x11); io_wait();
    outb(K_PIC1_DATA, 0x20); io_wait();   /* master IRQ0-7 -> 0x20..0x27 */
    outb(K_PIC2_DATA, 0x28); io_wait();   /* slave  IRQ8-15 -> 0x28..0x2F */
    outb(K_PIC1_DATA, 0x04); io_wait();
    outb(K_PIC2_DATA, 0x02); io_wait();
    outb(K_PIC1_DATA, 0x01); io_wait();
    outb(K_PIC2_DATA, 0x01); io_wait();
    outb(K_PIC1_DATA, 0xFF);             /* 全关，按需逐个开 */
    outb(K_PIC2_DATA, 0xFF);
}
void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(K_PIC2_CMD, 0x20);
    outb(K_PIC1_CMD, 0x20);
}
void irq_enable(uint8_t irq) {
    uint16_t p = irq < 8 ? K_PIC1_DATA : K_PIC2_DATA;
    uint8_t b = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(p, (uint8_t)(inb(p) & ~(1u << b)));
}
void irq_disable(uint8_t irq) {
    uint16_t p = irq < 8 ? K_PIC1_DATA : K_PIC2_DATA;
    uint8_t b = irq < 8 ? irq : (uint8_t)(irq - 8);
    outb(p, (uint8_t)(inb(p) | (1u << b)));
}

void cpu_init(void) {
    tss_setup();
    for (uint32_t i = 0; i < K_IDT_ENTRIES; i++) idt_set((uint8_t)i, isr_stub_table[i], 0);
    idt_set(0x80, isr_stub_table[0x80], 3);   /* int 0x80 系统调用，DPL=3 允许 ring3 */
    idt_load();
    pic_remap();
    g_kernel_pml4_phys = read_cr3();
}

/* ---- 异常/IRQ 分发 ---- */
static const char *exname(uint32_t v) {
    static const char *n[] = {
        "DE","DB","NMI","BP","OF","BR","UD","NM","DF","CSO","TS","NP","SS","GP","PF","RES",
        "MF","AC","MC","XF","RES","CP","RES","RES","RES","RES","RES","RES","HV","VC","SX","RES"
    };
    return v < 32 ? n[v] : "IRQ/INT";
}

void isr_c_handler(regs_t *r) {
    uint32_t v = (uint32_t)r->vector;
    if (v < 32) {
        g_fault_count++;
        con_printf("\n[KERNEL FAULT] vec=%u(%s) err=0x%x RIP=0x%x CR2=0x%x CS=0x%x\n",
                   v, exname(v), (uint32_t)r->errcode, (uint32_t)r->rip,
                   (uint32_t)read_cr2(), (uint32_t)r->cs);
        /* 致命异常：如实停机，不静默吞掉 */
        for (;;) { k_cli(); k_hlt(); }
    } else if (v == 0x20) {              /* IRQ0 PIT */
        g_ticks++;
        /* 用户态被时钟打断时，由抢占式调度器保存当前整帧并轮转下一个任务 */
        if (g_current && (r->cs & 3) == 3) sched_tick(r);
        pic_eoi(0);
    } else if (v == 0x21) {              /* IRQ1 PS/2 键盘 */
        uint8_t sc = inb(K_PS2_DATA);
        /* 由 devices.c 注册的翻译器处理 */
        extern void keyboard_irq_scancode(uint8_t sc);
        keyboard_irq_scancode(sc);
        pic_eoi(1);
    } else if (v == 0x24) {              /* IRQ4 COM1 串口 RX */
        int c = (int)inb(K_SERIAL_COM1);
        con_irq_serial((char)c);
        pic_eoi(4);
    } else if (v == 0x80) {              /* int 0x80 系统调用 */
        r->rax = (uint64_t)(uint64_t)(int64_t)syscall_dispatch(r);
    } else if (v >= 0x20 && v <= 0x2F) {
        pic_eoi((uint8_t)(v - 0x20));
    }
}
