/*
 * AetherOS 用户态程序链接脚本
 * 独立 ELF64 可执行文件，加载基址 K_USER_LOAD_VADDR (4 MiB)。
 * 内核 ELF 加载器按 PT_LOAD 段映射为 U/S=1 用户页后 iretq 进 ring3。
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text : ALIGN(0x1000) { *(.text .text.*) }
    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }
    .data : ALIGN(0x1000) { *(.data .data.*) }
    .bss : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame) *(.note.GNU-stack) *(.comment) *(.note .note.*) }
}
