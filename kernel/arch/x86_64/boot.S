/*
 * AetherOS Kernel · Multiboot1 引导 + IA-32e 长模式切换
 *
 * 参考（见 docs/03-KERNEL-STUDY.md）：
 *   OSDev "Bare Bones" multiboot header / 1 MiB 加载
 *   OSDev "Setting Up Long Mode"：PAE -> EFER.LME -> CR0.PG -> 64 位 GDT -> 远跳
 *   GitHub milescoviello/OS-DEV：QEMU -kernel 只吃 32 位 ELF 容器，
 *     因此链接成 ELF64 后用 objcopy -O elf32-i386 重框，内部 64 位机器码不变。
 *
 * 本文件以 GNU as（AT&T 语法）汇编：先用 --32 汇编 boot 部分为 32 位代码，
 * 64 位部分由 .code64 段单独汇编，最后合并进同一个 ELF64 镜像。
 * 数值常量统一来自 aether_kconfig.h（由 build.sh 生成 boot_config.h 给汇编器）。
 */
#include "boot_config.h"

.section .multiboot, "a", @progbits
.align 4
.long K_MB_HEADER_MAGIC
.long K_MB_HEADER_FLAGS
.long -(K_MB_HEADER_MAGIC + K_MB_HEADER_FLAGS)

/*
 * ---- 32 位引导数据与临时页表（4 KiB 对齐） ----
 * 必须放在随镜像加载的 progbits 段，且 **不在 __bss_start..__bss_end 范围内**：
 * k_entry64 会 rep stosb 清 BSS，若页表落在 BSS 内会把正在使用的 PDPT 自己清零，
 * 立刻 #PF -> #DF -> triple fault（实测日志：CR2=0x104000, RDI=0x104000）。
 */
.section .boot_scratch, "aw", @progbits
.align 4096
.global boot_pml4
boot_pml4:  .space 4096
.global boot_pdpt
boot_pdpt:  .space 4096
.global boot_pd
boot_pd:    .space 4096
.align 16
boot_stack: .space K_BOOT_STACK_BYTES
.global boot_stack_top
boot_stack_top:

.section .boot_data, "aw", @progbits
.global mb_magic_save
mb_magic_save: .long 0
.global mb_info_save
mb_info_save:  .long 0

/* ---- GDT（64 位）---- */
.section .data.gdt, "aw", @progbits
.align 16
.global gdt64
gdt64:
    .quad 0x0000000000000000          /* 0x00 null */
    .quad 0x00AF9A000000FFFF          /* 0x08 kernel code (L=1, DPL=0) */
    .quad 0x00AF92000000FFFF          /* 0x10 kernel data */
    .quad 0x00AFFA000000FFFF          /* 0x18 user code   (L=1, DPL=3) */
    .quad 0x00AFF2000000FFFF          /* 0x20 user data   (DPL=3) */
    .quad 0x0000000000000000          /* 0x28 TSS low  (C 侧填充) */
    .quad 0x0000000000000000          /* 0x30 TSS high (x86_64 系统描述符 16B) */
gdt64_end:
.global gdt64_ptr
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

/* ---- 32 位入口 ---- */
.section .boot_text, "ax", @progbits
.code32
.global _start
_start:
    cli
    movl $boot_stack_top, %esp
    movl %eax, mb_magic_save         /* 0x2BADB002 */
    movl %ebx, mb_info_save          /* multiboot_info* */

    /* 校验 long mode 可用：CPUID.80000001:EDX.LM(bit29) */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .Lno_longmode
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .Lno_longmode

    /* 清页表区（3 * 4096 = 12288 字节） */
    movl $boot_pml4, %edi
    xorl %eax, %eax
    movl $(3 * 4096 / 4), %ecx
    rep stosl

    /* PML4[0] -> PDPT, PDPT[0] -> PD
     * 上层页表项必须带 U/S=1（K_PTE_USER_FLAG）：x86 在 PML4/PDPT/PD/PT 每一级都查
     * U/S 位，上层若 U/S=0，ring3 根本无法沿页表走到任何用户页（#PF 保护违规）。
     * ring0 访问无视 U/S 位，故内核运行不受影响；内核自身的 2 MiB 大页（PD[0] 等）
     * 仍用 K_PDE_HUGE_FLAG(无 US) 保持 US=0，用户态访问会被挡在 PD 层 -> 隔离成立。 */
    movl $boot_pml4, %edi
    movl $(boot_pdpt + K_PTE_USER_FLAG), (%edi)
    movl $boot_pdpt, %edi
    movl $(boot_pd + K_PTE_USER_FLAG), (%edi)

    /* PD[0..511] -> 2 MiB 大页，identity map 低 1 GiB */
    movl $boot_pd, %edi
    xorl %ecx, %ecx
.Lfill_pd:
    movl %ecx, %eax
    shll $21, %eax                   /* i * 2 MiB */
    orl $(K_PDE_HUGE_FLAG), %eax
    movl %eax, (%edi, %ecx, 8)
    movl $0, 4(%edi, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jb .Lfill_pd

    /* CR4.PAE = 1 */
    movl %cr4, %eax
    orl $(1 << K_CR4_PAE_BIT), %eax
    movl %eax, %cr4

    /* CR3 = PML4 */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* EFER.LME = 1 */
    movl $(K_EFER_MSR), %ecx
    rdmsr
    orl $(1 << K_EFER_LME_BIT), %eax
    wrmsr

    /* CR0.PG | CR0.PE */
    movl %cr0, %eax
    orl $(1 << K_CR0_PG_BIT), %eax
    orl $1, %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $k_entry64           /* 远跳进 64 位代码段 */

.Lno_longmode:
    /* 长模式不可用：串口输出 '!' 后停机（如实失败，不假装成功） */
    movb $'!', %al
    movw $(K_SERIAL_COM1), %dx
    outb %al, %dx
    cli
.Lhang32:
    hlt
    jmp .Lhang32

/* ---- 64 位入口 ---- */
.section .text.entry, "ax", @progbits
.code64
.global k_entry64
k_entry64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movabsq $boot_stack_top, %rsp
    xorl %ebp, %ebp
    cld

    /* 清 BSS（C 全局必须为零） */
    movabsq $__bss_start, %rdi
    movabsq $__bss_end, %rcx
    subq %rdi, %rcx
    xorl %eax, %eax
    rep stosb

    movl mb_magic_save, %edi
    movl mb_info_save, %esi
    call kmain
    cli
.Lhang64:
    hlt
    jmp .Lhang64

/* 让汇编器把 GDT 段标记为可分配，避免被 ld 丢弃 */
.section .note.GNU-stack, "", @progbits
