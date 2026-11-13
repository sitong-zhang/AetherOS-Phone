/*
 * AetherOS Kernel · 中断桩 / 系统调用入口 / ring3 切换
 *
 * 栈帧统一设计（关键）：CPU 压 rip/cs/rflags(/rsp/ss)，
 *   - 无 errcode 异常：桩压 dummy 0 + vector
 *   - 有 errcode 异常：CPU 已压 errcode，桩只压 vector
 * 两者栈帧完全一致：vector, errcode, rip, cs, rflags[, rsp, ss]，
 * iretq 前统一 add rsp,16 即可。regs_t 的字段顺序与本文件 push 顺序严格对应。
 */
#include "boot_config.h"

.section .text, "ax", @progbits
.code64

/* 保存寄存器，使 rsp 指向 regs_t.r15（最低地址字段） */
.macro SAVE_REGS
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm
.macro RESTORE_REGS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

.global isr_common
isr_common:
    SAVE_REGS
    movq %rsp, %rdi
    cld
    call isr_c_handler
    RESTORE_REGS
    addq $16, %rsp            /* 丢弃 vector + errcode */
    iretq

/* ---- 生成 256 个中断桩 ---- */
.altmacro
.macro ISR_ONE v
    .if (\v == 8) || (\v == 10) || (\v == 11) || (\v == 12) || (\v == 13) || \
        (\v == 14) || (\v == 17) || (\v == 21) || (\v == 29) || (\v == 30)
        .global isr_stub_\v
        isr_stub_\v:
            pushq $\v
            jmp isr_common
    .else
        .global isr_stub_\v
        isr_stub_\v:
            pushq $0
            pushq $\v
            jmp isr_common
    .endif
.endm
.set V, 0
.rept 256
    ISR_ONE %V
    .set V, V + 1
.endr

/* ---- 中断桩指针表（C 侧 idt_set 用）----
 * 注意：.altmacro 下 `.quad isr_stub_%V` 里的 % 会被当作模运算符（实测报错），
 * 必须同样走「宏参数 \v 拼接」的方式生成。 */
.macro ISR_TABLE_ENT v
    .quad isr_stub_\v
.endm
.section .data, "aw", @progbits
.align 8
.global isr_stub_table
isr_stub_table:
.set V, 0
.rept 256
    ISR_TABLE_ENT %V
    .set V, V + 1
.endr

/* ---- 从 ring0 构造 iretq 帧切入 ring3 ---- */
.section .text, "ax", @progbits
.global task_enter_user
task_enter_user:
    /* rdi = 用户入口, rsi = 用户栈顶 */
    movq $0x23, %rax          /* 用户数据段 0x20 | RPL3 */
    pushq %rax                /* SS */
    pushq %rsi                /* RSP */
    pushfq
    popq %rax
    orq $0x202, %rax          /* IF | 保留位 bit1 */
    pushq %rax                /* RFLAGS */
    movq $0x1B, %rax          /* 用户代码段 0x18 | RPL3 */
    pushq %rax                /* CS */
    pushq %rdi                /* RIP */
    /* 刷新 TLB：拆大页建的用户页(US=1)必须重载 CR3 才对旧 2MiB 内核大页项(U/S=0)
       失效，否则用户态在 0x400000 取指命中旧项 -> #PF(权限) */
    movq %cr3, %rax
    movq %rax, %cr3
    /* 清空通用寄存器，防内核数据泄漏到用户态 */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %rbp, %rbp
    xorq %r8,  %r8
    xorq %r9,  %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    iretq

.section .note.GNU-stack, "", @progbits
