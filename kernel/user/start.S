/*
 * AetherOS 用户态 init · ring3 入口与系统调用封装
 * 独立 ELF64，链接到 K_USER_LOAD_VADDR；由内核 ELF 加载器映射为 U/S=1 用户页。
 */
#include "boot_config.h"

.section .text
.code64
.global _start
_start:
    xorq %rbp, %rbp
    /* 内核在用户栈顶放了 argc=0/argv=NULL，这里按 SysV ABI 调 main */
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    call umain
    /* umain 返回码 -> SYS_EXIT（不返回） */
    movq %rax, %rdi
    movq $(K_SYS_EXIT), %rax
    int $0x80
    /* 不应到达 */
.Lu_hang:
    hlt
    jmp .Lu_hang

/* long syscall(long nr, long a1, long a2, long a3, long a4) */
.global usyscall
usyscall:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8,  %rcx
    movq %r9,  %r10
    int $0x80
    ret

.section .note.GNU-stack, "", @progbits
