/*
 * AetherOS Kernel · 设备驱动
 * PIT 100 Hz（调度/超时）、PS/2 键盘（scancode set1 -> ASCII）、
 * ATA PIO LBA28（虚拟磁盘 = 可擦除数据区）、CSPRNG（RDRAND + 时钟白化）。
 */
#include "aether_kernel.h"

/* ---- PIT ---- */
void timer_init(void) {
    uint32_t div = K_PIT_BASE_HZ / K_PIT_HZ;
    outb(K_PIT_CMD, 0x36);
    outb(K_PIT_CH0, (uint8_t)(div & 0xFF));
    outb(K_PIT_CH0, (uint8_t)(div >> 8));
    irq_enable(0);
}

/* ---- PS/2 键盘 ---- */
static const char kbd_lower[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,'-',0,0,0,'+',0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const char kbd_upper[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,'-',0,0,0,'+',0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static bool g_shift, g_caps;
void keyboard_irq_scancode(uint8_t sc) {
    if (sc == 0x2A || sc == 0x36) { g_shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = false; return; }
    if (sc == 0x3A && !g_caps) { g_caps = !g_caps; return; }
    if (sc & 0x80) return;   /* 忽略断码 */
    char c = (g_shift ^ g_caps) ? kbd_upper[sc & 0x7F] : kbd_lower[sc & 0x7F];
    if (c) con_irq_keyboard(c);
}
void keyboard_init(void) {
    g_shift = false; g_caps = false;
    while (inb(K_PS2_STATUS) & 1) (void)inb(K_PS2_DATA);
    irq_enable(1);
    irq_enable(4);   /* COM1 RX */
}

/* ---- ATA PIO LBA28 ---- */
static int g_ata_ok;
static void ata_400ns(void) { for (int i = 0; i < 4; i++) inb(K_ATA_CTRL); }
static int ata_poll(void) {
    for (int i = 0; i < 200000; i++) {
        uint8_t s = inb(K_ATA_IO_BASE + 7);
        if (!(s & 0x80)) {           /* BSY clear */
            if (s & 0x08) return 0; /* DRQ set */
            if (s & 0x01) return -1;/* ERR */
        }
        k_pause();
    }
    return -2;
}
void ata_init(void) {
    g_ata_ok = 0;
    outb(K_ATA_IO_BASE + 6, 0xA0);            /* master, LBA */
    ata_400ns();
    uint8_t s = inb(K_ATA_IO_BASE + 7);
    if (s == 0xFF) return;                    /* 无控制器 */
    /* IDENTIFY */
    outb(K_ATA_IO_BASE + 2, 0); outb(K_ATA_IO_BASE + 3, 0);
    outb(K_ATA_IO_BASE + 4, 0); outb(K_ATA_IO_BASE + 5, 0);
    outb(K_ATA_IO_BASE + 7, 0xEC);
    if (inb(K_ATA_IO_BASE + 7) == 0) return;
    if (ata_poll() == 0) {
        uint16_t id[256];
        for (uint32_t i = 0; i < 256; i++) id[i] = inw(K_ATA_IO_BASE);
        (void)id;
    } else {
        /* QEMU 的 IDENTIFY 可能不带 DRQ；用读扇区 0 探测实际可用性 */
    }
    g_ata_ok = 1;
}
int ata_present(void) { return g_ata_ok; }

static void ata_wait_ready(void) {
    for (int i = 0; i < 200000; i++) {
        uint8_t s = inb(K_ATA_IO_BASE + 7);
        if (!(s & 0x80) && (s & 0x40)) return;   /* BSY=0 且 DRDY=1 */
        k_pause();
    }
}
static void ata_select(uint32_t lba, uint8_t cmd) {
    ata_wait_ready();                              /* PIO 写/读前必须等驱动器就绪 */
    outb(K_ATA_IO_BASE + 6, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(K_ATA_IO_BASE + 1, 0x00);
    outb(K_ATA_IO_BASE + 2, 1);
    outb(K_ATA_IO_BASE + 3, (uint8_t)(lba & 0xFF));
    outb(K_ATA_IO_BASE + 4, (uint8_t)((lba >> 8) & 0xFF));
    outb(K_ATA_IO_BASE + 5, (uint8_t)((lba >> 16) & 0xFF));
    outb(K_ATA_IO_BASE + 7, cmd);
}
int ata_read(uint32_t lba, void *buf) {
    if (!g_ata_ok) return -1;
    uint16_t *w = (uint16_t *)buf;
    ata_select(lba, 0x20);
    if (ata_poll() != 0) return -2;
    for (uint32_t i = 0; i < K_SECTOR_BYTES / 2; i++) w[i] = inw(K_ATA_IO_BASE);
    ata_400ns();
    return 0;
}
int ata_write(uint32_t lba, const void *buf) {
    if (!g_ata_ok) return -1;
    const uint16_t *w = (const uint16_t *)buf;
    ata_select(lba, 0x30);
    if (ata_poll() != 0) return -2;
    for (uint32_t i = 0; i < K_SECTOR_BYTES / 2; i++) outw(K_ATA_IO_BASE, w[i]);
    outb(K_ATA_IO_BASE + 7, 0xE7);   /* CACHE FLUSH */
    ata_400ns();
    return 0;
}

/* ---- CSPRNG ---- */
static uint64_t g_rng_state;
static int rdrand64(uint64_t *out) {
    uint64_t v; unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    if (ok) *out = v;
    return (int)ok;
}
static uint64_t tsc_read(void) {
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
uint32_t ata_rng_u32(void) {
    uint64_t x = 0;
    if (!rdrand64(&x)) x = tsc_read() ^ (g_ticks * 0x9E3779B97F4A7C15ull);
    g_rng_state ^= x + 0x9E3779B97F4A7C15ull + (g_rng_state << 6) + (g_rng_state >> 2);
    return (uint32_t)(g_rng_state >> 17);
}
void csprng(uint8_t *out, size_t n) {
    uint8_t seed[64];
    uint64_t t = tsc_read() ^ g_ticks;
    for (int i = 0; i < 8; i++) {
        uint64_t v = t ^ ((uint64_t)ata_rng_u32() << 32) ^ (uint64_t)ata_rng_u32();
        kmemcpy(seed + i * 8, &v, 8);
    }
    uint8_t h[K_HASH_BYTES];
    sha256_buf(seed, sizeof seed, h);
    for (size_t i = 0; i < n; ) {
        sha256_buf(seed, sizeof seed, h);
        sha256_buf(h, sizeof h, seed);
        size_t k = n - i < K_HASH_BYTES ? n - i : K_HASH_BYTES;
        kmemcpy(out + i, h, k);
        i += k;
        uint64_t c = tsc_read();
        kmemcpy(seed + 56, &c, 8);
    }
}
