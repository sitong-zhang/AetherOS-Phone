/*
 * Multiboot 1 规范结构（来自公开规范，非 Linux 头文件）。
 * QEMU 的 -kernel 内置 multiboot 加载器交出 EAX=0x2BADB002、EBX=本结构物理地址。
 * 参考 docs/03-KERNEL-STUDY.md [[1]][[9]]。
 */
#ifndef AETHER_MULTIBOOT_H
#define AETHER_MULTIBOOT_H
#include "aether_types.h"

#define MB_FLAG_MEM      0x00000001u
#define MB_FLAG_BOOTDEV  0x00000002u
#define MB_FLAG_CMDLINE  0x00000004u
#define MB_FLAG_MODS     0x00000008u
#define MB_FLAG_MMAP     0x00000040u
#define MB_FLAG_FB       0x00001000u
#define MB_MMAP_AVAILABLE 1u
#define MB_MMAP_RESERVED  2u

typedef struct K_PACKED {
    uint32_t size;
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mb_mmap_entry_t;

typedef struct K_PACKED {
    uint32_t mod_start, mod_end, cmdline, pad;
} mb_module_t;

typedef struct K_PACKED {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch, framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp, framebuffer_type;
    uint8_t  color_info[6];
} mb_info_t;

#endif /* AETHER_MULTIBOOT_H */
