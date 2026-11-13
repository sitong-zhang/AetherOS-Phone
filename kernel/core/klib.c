/*
 * AetherOS Kernel · freestanding C 运行库
 * GCC 在 -ffreestanding 下仍可能生成 memset/memcpy/memmove 调用，必须自提供。
 * 不 #include 任何系统头，不链接 libc。
 */
#include "aether_kernel.h"

void *kmemset(void *d, int c, size_t n) {
    uint8_t *p = (uint8_t *)d;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return d;
}
void *kmemcpy(void *d, const void *s, size_t n) {
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) p[i] = q[i];
    return d;
}
void *kmemmove(void *d, const void *s, size_t n) {
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s;
    if (p == q || n == 0) return d;
    if (p < q) { for (size_t i = 0; i < n; i++) p[i] = q[i]; }
    else { for (size_t i = n; i > 0; i--) p[i - 1] = q[i - 1]; }
    return d;
}
int kmemcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) { if (p[i] != q[i]) return (int)p[i] - (int)q[i]; }
    return 0;
}
size_t kstrlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int kstrcmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
}
int kstrncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    }
    return 0;
}
char *kstrncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
int katoi(const char *s) {
    int v = 0, sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}
void khex(char *out, const uint8_t *data, size_t n) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[data[i] >> 4];
        out[i * 2 + 1] = h[data[i] & 15];
    }
    out[n * 2] = 0;
}

/* ---- 精简 vsnprintf：%s %d %u %x %X %c %p %%，支持宽度 0 填充与长整型 ---- */
typedef int (*putc_fn)(void *, char);
struct out_ctx { char *buf; size_t cap, pos; };
static int out_putc(void *p, char c) {
    struct out_ctx *o = (struct out_ctx *)p;
    if (o->pos + 1 < o->cap) o->buf[o->pos] = c;
    o->pos++;
    return 1;
}
static void emit_str(struct out_ctx *o, const char *s) {
    while (*s) out_putc(o, *s++);
}
static void emit_uint(struct out_ctx *o, uint64_t v, unsigned base, int upper,
                      int width, char pad) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    char tmp[32]; int n = 0;
    do { tmp[n++] = upper ? up[v % base] : lo[v % base]; v /= base; } while (v && n < 32);
    while (n < width && n < 32) tmp[n++] = pad;
    while (n > 0) out_putc(o, tmp[--n]);
}
static void emit_int(struct out_ctx *o, int64_t v, int width, char pad) {
    uint64_t u;
    if (v < 0) { out_putc(o, '-'); u = (uint64_t)(-v); }
    else u = (uint64_t)v;
    emit_uint(o, u, 10, 0, width, pad);
}
int kvsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap) {
    struct out_ctx o = { buf, n, 0 };
    if (!buf || !n) return 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out_putc(&o, *p); continue; }
        p++;
        char pad = ' '; int width = 0; int is_long = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l') { is_long = 1; p++; }
        switch (*p) {
            case 's': emit_str(&o, __builtin_va_arg(ap, const char *)); break;
            case 'c': out_putc(&o, (char)__builtin_va_arg(ap, int)); break;
            case 'd': case 'i':
                emit_int(&o, is_long ? __builtin_va_arg(ap, int64_t)
                                     : (int64_t)__builtin_va_arg(ap, int), width, pad);
                break;
            case 'u':
                emit_uint(&o, is_long ? __builtin_va_arg(ap, uint64_t)
                                      : (uint64_t)__builtin_va_arg(ap, uint32_t),
                          10, 0, width, pad);
                break;
            case 'x': case 'X':
                emit_uint(&o, is_long ? __builtin_va_arg(ap, uint64_t)
                                      : (uint64_t)__builtin_va_arg(ap, uint32_t),
                          16, *p == 'X', width, pad);
                break;
            case 'p':
                out_putc(&o, '0'); out_putc(&o, 'x');
                emit_uint(&o, __builtin_va_arg(ap, uint64_t), 16, 0, width, pad);
                break;
            case '%': out_putc(&o, '%'); break;
            default: out_putc(&o, '%'); out_putc(&o, *p); break;
        }
    }
    buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}
int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    __builtin_va_end(ap);
    return r;
}
