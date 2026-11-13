/*
 * AetherOS Kernel · freestanding 基础类型
 * 内核完全不链接 libc（v2 §5.6 第四门）；本头只依赖编译器内建，不 #include 任何系统头。
 */
#ifndef AETHER_TYPES_H
#define AETHER_TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef uint64_t            size_t;
typedef int64_t             ssize_t;
typedef uint64_t            uintptr_t;
typedef int                 bool;

#define true   1
#define false  0
#define NULL   ((void *)0)

#define K_PACKED   __attribute__((packed))
#define K_NORETURN __attribute__((noreturn))
#define K_ALIGN(x) __attribute__((aligned(x)))

/* 端口 I/O */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r; __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void io_wait(void) { outb(0x80, 0); }

static inline void k_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void k_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void k_hlt(void) { __asm__ volatile("hlt"); }
static inline void k_pause(void) { __asm__ volatile("pause"); }

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}
static inline uint64_t read_cr2(void) {
    uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory");
}
static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}
static inline uint64_t read_rflags(void) {
    uint64_t v; __asm__ volatile("pushfq; popq %0" : "=r"(v) :: "memory"); return v;
}

/* CPUID（自检用） */
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t aa = leaf, bb = 0, cc = 0, dd = 0;
    __asm__ volatile("cpuid" : "+a"(aa), "=b"(bb), "=c"(cc), "=d"(dd));
    if (a) { *a = aa; }
    if (b) { *b = bb; }
    if (c) { *c = cc; }
    if (d) { *d = dd; }
}

#endif /* AETHER_TYPES_H */
