/*
 * AetherOS Kernel · ELF64 加载器 + ring3 用户态启动
 * 参考 OSDev "Loading ELF Binaries"：校验 e_ident、遍历 PT_LOAD、
 * 按 p_filesz 拷贝、p_memsz-p_filesz 清零，映射用户页（U/S=1），构造用户栈，
 * 最后 task_enter_user() 用 iretq 从 ring0 切到 ring3。
 */
#include "aether_kernel.h"

typedef struct K_PACKED {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;
typedef struct K_PACKED {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr;
    uint64_t p_filesz, p_memsz, p_align;
} elf64_phdr_t;
#define PT_LOAD 1u
#define ET_EXEC 2u
#define EM_X86_64 62u

/*
 * 把 ELF 加载进当前页表（共享内核页表）并映射独立的用户栈，返回入口与栈顶。
 * 不进入 ring3 —— 由调用方决定何时/是否进入（单进程直进，或多进程交调度器）。
 */
int elf_load(const void *image, size_t len, uint64_t load_vaddr,
             uint64_t stack_top, uint64_t stack_bytes,
             char *msg, size_t n, uint64_t *out_entry, uint64_t *out_stack) {
    (void)load_vaddr;   /* ELF 段自带正确 vaddr（init@0x400000 / task2@0x600000），此处 identity 映射 */
    if (!image || len < sizeof(elf64_ehdr_t)) {
        return ksnprintf(msg, n, "ELF FAIL: image too small\n"), -1;
    }
    const uint8_t *img = (const uint8_t *)image;
    elf64_ehdr_t eh; kmemcpy(&eh, img, sizeof eh);
    if (!(eh.e_ident[0] == 0x7F && eh.e_ident[1] == 'E' &&
          eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F')) {
        return ksnprintf(msg, n, "ELF FAIL: bad magic\n"), -2;
    }
    if (eh.e_ident[4] != 2 || eh.e_machine != EM_X86_64 || eh.e_type != ET_EXEC) {
        return ksnprintf(msg, n, "ELF FAIL: not ELF64 x86-64 EXEC\n"), -3;
    }
    if (eh.e_phnum == 0 || eh.e_phentsize < sizeof(elf64_phdr_t)) {
        return ksnprintf(msg, n, "ELF FAIL: no program headers\n"), -4;
    }
    uint64_t lo = ~0ull, hi = 0;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t ph;
        uint64_t off = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        if (off + sizeof ph > len) return ksnprintf(msg, n, "ELF FAIL: phdr out of range\n"), -5;
        kmemcpy(&ph, img + off, sizeof ph);
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_offset + ph.p_filesz > len) return ksnprintf(msg, n, "ELF FAIL: segment out of range\n"), -6;
        if (ph.p_vaddr < lo) lo = ph.p_vaddr;
        if (ph.p_vaddr + ph.p_memsz > hi) hi = ph.p_vaddr + ph.p_memsz;
        /* 每个 PT_LOAD 段按 4 KiB 映射为 U/S=1 用户页（identity） */
        uint64_t b = ph.p_vaddr & ~((uint64_t)K_PAGE_SIZE - 1);
        uint64_t e = (ph.p_vaddr + ph.p_memsz + K_PAGE_SIZE - 1) & ~((uint64_t)K_PAGE_SIZE - 1);
        for (uint64_t p = b; p < e; p += K_PAGE_SIZE)
            if (vmm_map(p, p, 0x02 | 0x04) != 0)
                return ksnprintf(msg, n, "ELF FAIL: vmm_map out of memory\n"), -7;
        /* 拷贝 + 清零 BSS */
        kmemcpy((void *)(uintptr_t)ph.p_vaddr, img + ph.p_offset, (size_t)ph.p_filesz);
        if (ph.p_memsz > ph.p_filesz)
            kmemset((void *)(uintptr_t)(ph.p_vaddr + ph.p_filesz), 0,
                    (size_t)(ph.p_memsz - ph.p_filesz));
    }
    /* 用户栈：单独映射，U/S=1（栈顶须低于 load_vaddr，避免与代码段重叠） */
    uint64_t stack_bottom = stack_top - stack_bytes;
    for (uint64_t p = stack_bottom; p < stack_top; p += K_PAGE_SIZE)
        if (vmm_map(p, p, 0x02 | 0x04) != 0)
            return ksnprintf(msg, n, "ELF FAIL: stack map failed\n"), -8;
    /* 在用户栈顶放一个伪 argc=0 帧，保持 SysV ABI 栈对齐 */
    uint64_t *usp = (uint64_t *)(stack_top - 16);
    usp[0] = 0; usp[1] = 0;
    (void)lo; (void)hi;
    ksnprintf(msg, n,
        "ELF OK: entry=0x%x phnum=%u user_range=0x%x..0x%x stack=0x%x\n",
        (uint32_t)eh.e_entry, eh.e_phnum, (uint32_t)lo, (uint32_t)hi,
        (uint32_t)stack_top);
    con_write(msg);
    {
        char d[96];
        uint64_t fl = vmm_pte_flags(eh.e_entry);
        ksnprintf(d, sizeof d, "DIAG PTE(0x%llx)=0x%llx P=%d RW=%d US=%d\n",
                  (unsigned long long)eh.e_entry, (unsigned long long)fl,
                  (int)(fl & 1), (int)((fl >> 1) & 1), (int)((fl >> 2) & 1));
        con_write(d);
    }
    *out_entry = eh.e_entry;
    *out_stack = (uint64_t)(uintptr_t)usp;
    return 0;
}

int elf_load_and_run(const void *image, size_t len, char *msg, size_t n) {
    uint64_t e, s;
    int rc = elf_load(image, len, K_USER_LOAD_VADDR, K_USER_STACK_TOP,
                      K_USER_STACK_BYTES, msg, n, &e, &s);
    if (rc) return rc;
    con_write("-> entering ring3 (single process)\n");
    k_sti();
    task_enter_user(e, s);
    return 0;   /* 用户态通过 SYS_EXIT 回到这里（iretq 不返回，实际由独立路径处理） */
}
