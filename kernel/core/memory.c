/*
 * AetherOS Kernel · 内存管理
 * PMM：基于 multiboot mmap 的 4 KiB 帧位图；VMM：4 级页表映射（含把 2 MiB 大页
 * 拆成 4 KiB 用户页并置 U/S 位，供 ring3 用户态使用）；堆：bump + 简易空闲链。
 * identity map 低 1 GiB 由 boot.S 建立，内核全部地址都在这个范围。
 */
#include "aether_kernel.h"

extern uint8_t __kernel_phys_end[];

#define PMM_MAX_PAGES (K_PMM_MAX_MB * 256u)     /* 512 MiB / 4 KiB */
static uint8_t g_bitmap[PMM_MAX_PAGES / 8];
static uint64_t g_total_pages, g_free_pages, g_next_hint;

static void bmp_set(uint64_t i, int used) {
    if (i >= PMM_MAX_PAGES) return;
    uint8_t *p = &g_bitmap[i >> 3];
    uint8_t m = (uint8_t)(1u << (i & 7));
    if (used) *p |= m; else *p &= (uint8_t)~m;
}
static int bmp_get(uint64_t i) {
    if (i >= PMM_MAX_PAGES) return 1;
    return (g_bitmap[i >> 3] >> (i & 7)) & 1;
}

void pmm_init(mb_info_t *mbi) {
    kmemset(g_bitmap, 0xFF, sizeof g_bitmap);   /* 默认全占用，逐个放开可用区 */
    g_total_pages = 0; g_free_pages = 0; g_next_hint = 0;
    uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_phys_end;
    if (!(mbi->flags & MB_FLAG_MMAP)) {
        /* 无 mmap：保守使用 mem_upper（KiB） */
        uint64_t top = (uint64_t)mbi->mem_upper * 1024u;
        for (uint64_t i = 0; i * K_PAGE_SIZE < top && i < PMM_MAX_PAGES; i++) {
            bmp_set(i, 0); g_total_pages++; g_free_pages++;
        }
    } else {
        mb_mmap_entry_t *e = (mb_mmap_entry_t *)(uint64_t)mbi->mmap_addr;
        uint8_t *end = (uint8_t *)e + mbi->mmap_length;
        while ((uint8_t *)e + sizeof(mb_mmap_entry_t) <= end) {
            if (e->type == MB_MMAP_AVAILABLE) {
                uint64_t b = (e->base + K_PAGE_SIZE - 1) & ~((uint64_t)K_PAGE_SIZE - 1);
                uint64_t t = (e->base + e->length) & ~((uint64_t)K_PAGE_SIZE - 1);
                for (uint64_t p = b; p + K_PAGE_SIZE <= t && (p >> 12) < PMM_MAX_PAGES; p += K_PAGE_SIZE) {
                    bmp_set(p >> 12, 0);
                    g_total_pages++; g_free_pages++;
                }
            }
            e = (mb_mmap_entry_t *)((uint8_t *)e + e->size + 4);
            if (e->size == 0) break;
        }
    }
    /* 内核镜像自身 + 低 1 MiB（BIOS/VGA/引导页表）标记为已占用 */
    for (uint64_t p = 0; p < kernel_end + K_HEAP_BYTES; p += K_PAGE_SIZE) {
        uint64_t i = p >> 12;
        if (i < PMM_MAX_PAGES && !bmp_get(i)) { bmp_set(i, 1); g_free_pages--; }
    }
    /* 堆区从 K_HEAP_START 起，静态保留，不走 pmm_alloc */
}
void *pmm_alloc(void) {
    for (uint64_t i = g_next_hint; i < PMM_MAX_PAGES; i++) {
        if (!bmp_get(i)) {
            bmp_set(i, 1); g_free_pages--; g_next_hint = i + 1;
            uint8_t *p = (uint8_t *)(i * K_PAGE_SIZE);
            kmemset(p, 0, K_PAGE_SIZE);
            return p;
        }
    }
    return NULL;
}
void pmm_free(void *p) {
    uint64_t i = (uint64_t)(uintptr_t)p >> 12;
    if (i < PMM_MAX_PAGES && bmp_get(i)) { bmp_set(i, 0); g_free_pages++; if (i < g_next_hint) g_next_hint = i; }
}
uint64_t pmm_free_pages(void)  { return g_free_pages; }
uint64_t pmm_total_pages(void) { return g_total_pages; }

/* ---- VMM ---- */
#define PML4_IDX(a) (((a) >> 39) & 0x1FF)
#define PDPT_IDX(a) (((a) >> 30) & 0x1FF)
#define PD_IDX(a)   (((a) >> 21) & 0x1FF)
#define PT_IDX(a)   (((a) >> 12) & 0x1FF)
#define PE_ADDR(e)  ((e) & 0x000FFFFFFFFFF000ull)
#define PE_PRESENT  0x01ull
#define PE_RW       0x02ull
#define PE_US       0x04ull
#define PE_PS       0x80ull

static uint64_t *g_pml4;
static uint64_t g_cr3;

static uint64_t *table_at(uint64_t entry) { return (uint64_t *)(uintptr_t)PE_ADDR(entry); }

void vmm_init(void) {
    g_cr3 = read_cr3();
    g_pml4 = (uint64_t *)(uintptr_t)g_cr3;  /* identity map：CR3 值即页表物理/虚拟地址 */
}
uint64_t vmm_get_kernel_cr3(void) { return g_cr3; }

/* 诊断：返回某虚拟地址最终页表项（叶级；若中间级 huge 则返 huge 项）的低 12 位，
 * 用于确认 P/RW/US 位是否正确（定位 ring3 #PF 根因用）。 */
uint64_t vmm_pte_flags(uint64_t vaddr) {
    uint64_t *pml4 = (uint64_t *)(uintptr_t)read_cr3();
    uint64_t e = pml4[PML4_IDX(vaddr)];
    if (!(e & PE_PRESENT)) return 0;
    uint64_t *pdpt = table_at(e); e = pdpt[PDPT_IDX(vaddr)];
    if (!(e & PE_PRESENT)) return 0;
    uint64_t *pd = table_at(e); uint64_t pde = pd[PD_IDX(vaddr)];
    if (pde & PE_PS) return pde;            /* 2 MiB huge：直接看该项的权限位 */
    if (!(pde & PE_PRESENT)) return 0;
    uint64_t *pt = table_at(pde);
    return pt[PT_IDX(vaddr)];
}

/* 通用 4 KiB 映射：按需分配下级表。flags 为 PE_* 组合。 */
int vmm_map(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    vaddr &= ~((uint64_t)K_PAGE_SIZE - 1);
    paddr &= ~((uint64_t)K_PAGE_SIZE - 1);
    uint64_t *pml4 = (uint64_t *)(uintptr_t)read_cr3();
    uint64_t e = pml4[PML4_IDX(vaddr)];
    if (!(e & PE_PRESENT)) {
        uint64_t *t = (uint64_t *)pmm_alloc(); if (!t) return -1;
        pml4[PML4_IDX(vaddr)] = (uint64_t)(uintptr_t)t | PE_PRESENT | PE_RW | PE_US;
        e = pml4[PML4_IDX(vaddr)];
    }
    uint64_t *pdpt = table_at(e);
    e = pdpt[PDPT_IDX(vaddr)];
    if (!(e & PE_PRESENT)) {
        uint64_t *t = (uint64_t *)pmm_alloc(); if (!t) return -1;
        pdpt[PDPT_IDX(vaddr)] = (uint64_t)(uintptr_t)t | PE_PRESENT | PE_RW | PE_US;
        e = pdpt[PDPT_IDX(vaddr)];
    }
    uint64_t *pd = table_at(e);
    uint64_t pde = pd[PD_IDX(vaddr)];
    uint64_t *pt;
    if (pde & PE_PS) {
        /* 该 2 MiB 大页需要拆成 4 KiB 页表：先复制其内容为 512 个 4 KiB 项 */
        uint64_t base = PE_ADDR(pde);
        uint64_t *nt = (uint64_t *)pmm_alloc(); if (!nt) return -1;
        /* 拆成 512 个 4 KiB 叶项：必须继承调用方要求的 U/S 位（x86 分页每一级都查
         * U/S，任一级 U/S=0 都会让 ring3 访问触发 #PF）。boot 建的 2 MiB 大页是
         * 内核页(U/S=0)，这里漏 PE_US 正是此前 ring3 取指 #PF err=0x5 的根因。 */
        for (uint32_t i = 0; i < 512; i++)
            nt[i] = (base + (uint64_t)i * K_PAGE_SIZE) | PE_PRESENT | PE_RW | (flags & PE_US);
        pd[PD_IDX(vaddr)] = (uint64_t)(uintptr_t)nt | PE_PRESENT | PE_RW | PE_US;
        pt = nt;
    } else if (!(pde & PE_PRESENT)) {
        uint64_t *nt = (uint64_t *)pmm_alloc(); if (!nt) return -1;
        pd[PD_IDX(vaddr)] = (uint64_t)(uintptr_t)nt | PE_PRESENT | PE_RW | PE_US;
        pt = nt;
    } else pt = table_at(pde);
    pt[PT_IDX(vaddr)] = paddr | flags | PE_PRESENT;
    invlpg(vaddr);
    return 0;
}

/* 把 [vaddr, vaddr+bytes) 拆为 4 KiB 用户页（U/S=1, RW），identity 映射 */
int vmm_map_user_range(uint64_t vaddr, uint64_t bytes) {
    uint64_t b = vaddr & ~((uint64_t)K_PAGE_SIZE - 1);
    uint64_t e = (vaddr + bytes + K_PAGE_SIZE - 1) & ~((uint64_t)K_PAGE_SIZE - 1);
    for (uint64_t p = b; p < e; p += K_PAGE_SIZE)
        if (vmm_map(p, p, PE_RW | PE_US) != 0) return -1;
    return 0;
}

/* ---- 内核堆：bump 分配 + 按大小对齐，不做释放复用（内核生命周期内足够）---- */
static uint8_t *g_heap_cur;
static uint8_t *g_heap_end;
void *kheap_alloc(size_t n) {
    if (!g_heap_cur) {
        g_heap_cur = (uint8_t *)(uintptr_t)K_HEAP_START;
        g_heap_end = g_heap_cur + K_HEAP_BYTES;
    }
    n = (n + 15) & ~(size_t)15;
    if (g_heap_cur + n > g_heap_end) return NULL;
    void *p = g_heap_cur;
    g_heap_cur += n;
    kmemset(p, 0, n);
    return p;
}
void kheap_free(void *p) { (void)p; /* bump 堆不回收，如实说明 */ }
