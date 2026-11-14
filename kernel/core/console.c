/*
 * AetherOS Kernel · 控制台
 * 真实硬件：COM1 (0x3F8) 串口双向 + VGA text mode 0xB8000 彩色帧缓冲。
 * 输入来自串口 RX 中断与 PS/2 键盘 IRQ，汇入同一个环形缓冲。
 */
#include "aether_kernel.h"

static volatile uint16_t *const vga = (volatile uint16_t *)(uint64_t)K_VGA_FB_PHYS;
static uint8_t g_col, g_row, g_attr;
static bool g_ready;

#define IN_BUF 256
static volatile char g_in[IN_BUF];
static volatile uint32_t g_in_head, g_in_tail;

static void serial_init(void) {
    outb(K_SERIAL_COM1 + 1, 0x00);   /* 关中断 */
    outb(K_SERIAL_COM1 + 3, 0x80);   /* DLAB */
    outb(K_SERIAL_COM1 + 0, 0x01);   /* 115200 = 1153200/1 */
    outb(K_SERIAL_COM1 + 1, 0x00);
    outb(K_SERIAL_COM1 + 3, 0x03);   /* 8N1 */
    outb(K_SERIAL_COM1 + 2, 0xC7);   /* FIFO + 清空 */
    outb(K_SERIAL_COM1 + 4, 0x0B);   /* RTS/DSR + OUT2 */
    outb(K_SERIAL_COM1 + 1, 0x01);   /* 使能 RX 中断 */
}
static void serial_putc(char c) {
    int guard = 200000;
    while (!(inb(K_SERIAL_COM1 + 5) & 0x20) && guard-- > 0) k_pause();
    outb(K_SERIAL_COM1, (uint8_t)c);
}
static int serial_getc(void) {
    if (!(inb(K_SERIAL_COM1 + 5) & 0x01)) return -1;
    return (int)inb(K_SERIAL_COM1);
}
static void vga_cursor(void) {
    uint16_t pos = (uint16_t)(g_row * K_VGA_COLS + g_col);
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
}
static void vga_scroll(void) {
    for (uint32_t y = 1; y < K_VGA_ROWS; y++)
        for (uint32_t x = 0; x < K_VGA_COLS; x++)
            vga[(y - 1) * K_VGA_COLS + x] = vga[y * K_VGA_COLS + x];
    for (uint32_t x = 0; x < K_VGA_COLS; x++)
        vga[(K_VGA_ROWS - 1) * K_VGA_COLS + x] = (uint16_t)(g_attr << 8) | ' ';
}

void con_init(void) {
    serial_init();
    g_col = 0; g_row = 0; g_attr = 0x1F; g_ready = true;
    g_in_head = g_in_tail = 0;
    con_clear(g_attr);
    vga_cursor();
}
void con_clear(uint8_t attr) {
    g_attr = attr;
    for (uint32_t i = 0; i < K_VGA_COLS * K_VGA_ROWS; i++)
        vga[i] = (uint16_t)((uint16_t)attr << 8) | ' ';
    g_col = 0; g_row = 0;
    if (g_ready) vga_cursor();
}
void con_setattr(uint8_t attr) { g_attr = attr; }
void con_move(uint8_t col, uint8_t row) {
    g_col = col < K_VGA_COLS ? col : K_VGA_COLS - 1;
    g_row = row < K_VGA_ROWS ? row : K_VGA_ROWS - 1;
    if (g_ready) vga_cursor();
}
static void vga_putc(char c) {
    if (c == '\n') { g_col = 0; g_row++; }
    else if (c == '\r') { g_col = 0; }
    else if (c == '\b') { if (g_col) g_col--; vga[g_row * K_VGA_COLS + g_col] = ((uint16_t)g_attr << 8) | ' '; }
    else if (c == '\t') { g_col = (uint8_t)((g_col + 8) & ~7); }
    else { vga[g_row * K_VGA_COLS + g_col] = ((uint16_t)g_attr << 8) | (uint16_t)(uint8_t)c; g_col++; }
    if (g_col >= K_VGA_COLS) { g_col = 0; g_row++; }
    if (g_row >= K_VGA_ROWS) { vga_scroll(); g_row = K_VGA_ROWS - 1; }
}
void con_putc(char c) {
    unsigned long f;
    /* 抢占式多任务下，逐个字符发射期间关中断，避免两个任务的串口/VGA 写交错
     * 破坏共享的光标(g_col/g_row)状态导致画面乱码。单 CPU 下 cli 即足以互斥。 */
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f));
    serial_putc(c);
    if (g_ready) { vga_putc(c); vga_cursor(); }
    __asm__ volatile("pushq %0; popfq" :: "r"(f));
}
int con_write(const char *s) { int n = 0; while (s[n]) { con_putc(s[n]); n++; } return n; }
int con_printf(const char *fmt, ...) {
    char buf[1024];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    (void)kvsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    return con_write(buf);
}
void con_puts_at(uint8_t x, uint8_t y, const char *s, uint8_t attr) {
    uint32_t i = 0;
    while (s[i] && x + i < K_VGA_COLS) {
        vga[y * K_VGA_COLS + x + i] = ((uint16_t)attr << 8) | (uint16_t)(uint8_t)s[i];
        i++;
    }
}
void con_draw_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t attr, char border) {
    if (w < 2 || h < 2) return;
    for (uint8_t i = 0; i < w; i++) {
        con_puts_at((uint8_t)(x + i), y, &border, attr);
        con_puts_at((uint8_t)(x + i), (uint8_t)(y + h - 1), &border, attr);
    }
    for (uint8_t j = 1; j + 1 < h; j++) {
        con_puts_at(x, (uint8_t)(y + j), &border, attr);
        con_puts_at((uint8_t)(x + w - 1), (uint8_t)(y + j), &border, attr);
    }
}
static void in_push(char c) {
    uint32_t next = (g_in_head + 1) % IN_BUF;
    if (next == g_in_tail) return;    /* 满则丢，如实不假装 */
    g_in[g_in_head] = c;
    g_in_head = next;
}
void con_irq_serial(char c) { in_push(c); }
void con_irq_keyboard(char c) { in_push(c); }
void con_poll_serial_rx(void) {
    int c;
    while ((c = serial_getc()) >= 0) in_push((char)c);
}
int con_poll_char(char *out) {
    con_poll_serial_rx();
    if (g_in_tail == g_in_head) return 0;
    *out = g_in[g_in_tail];
    g_in_tail = (g_in_tail + 1) % IN_BUF;
    return 1;
}
int con_read_line(char *buf, size_t n, bool echo) {
    size_t len = 0;
    for (;;) {
        char c;
        if (!con_poll_char(&c)) { k_hlt(); continue; }  /* 等中断唤醒，省 TCG CPU */
        if (c == '\r' || c == '\n') { if (echo) { con_putc('\r'); con_putc('\n'); } break; }
        if (c == '\b' || c == 0x7F) {
            if (len) { len--; if (echo) { con_putc('\b'); con_putc(' '); con_putc('\b'); } }
            continue;
        }
        if (len + 1 < n && c >= 0x20 && c < 0x7F) {
            buf[len++] = c;
            if (echo) con_putc(c);
        }
    }
    buf[len] = 0;
    return (int)len;
}
