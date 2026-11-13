/*
 * AetherOS Kernel · 链接脚本
 * 1 MiB 物理加载（multiboot 约定）；identity map 低 1 GiB 覆盖全部内核地址。
 * multiboot header 必须位于文件前 8 KiB，故 .boot 段置顶且不自动对齐填充（ld -n）。
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;
    __kernel_start = .;

    .boot : {
        KEEP(*(.multiboot))
        KEEP(*(.boot_text))
        KEEP(*(.boot_data))
        /* 引导页表与引导栈：随镜像加载，绝不能落进 __bss_* 范围 */
        KEEP(*(.boot_scratch))
    }

    .text : ALIGN(0x1000) {
        __kernel_text_start = .;
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }
    __kernel_ro_end = .;                 /* 只读区末尾：之后是 .data/.bss（运行时可变） */

    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.data.gdt)
    }

    .bss : ALIGN(0x1000) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(0x1000);
        __bss_end = .;
    }

    __kernel_end = .;
    __kernel_phys_start = __kernel_start;
    __kernel_phys_end   = __kernel_end;

    /DISCARD/ : {
        *(.eh_frame) *(.eh_frame_hdr)
        *(.note.GNU-stack) *(.comment) *(.note .note.*)
    }
}
