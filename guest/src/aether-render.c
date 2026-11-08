/*
 * aether-render.c - AetherOS PC 渲染层实现
 *
 * 逐像素软件光栅化；ASCII 使用 DejaVu Sans Mono 8x16 点阵，
 * CJK 使用 Noto Sans CJK SC 渲染生成的 16x16 点阵（aether-font-cjk.h）。
 */
#include "aether-render.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether-font.h"
#include "aether-font-cjk.h"
#include "aether-logo.h"
#include "aether-glass.h"

static struct render_fb g;
static int g_clip_on = 0, g_cx = 0, g_cy = 0, g_cw = 0, g_ch = 0;
static int g_tx = 0, g_ty = 0;   /* 全局位移（统一转场/跟手动效） */
static unsigned char g_alpha = 0xFF;  /* 全局绘制 alpha */

void aether_render_translate(int dx, int dy) { g_tx = dx; g_ty = dy; }
void aether_render_set_alpha(unsigned char a) { g_alpha = a; }
unsigned char aether_render_alpha(void) { return g_alpha; }

void aether_render_set_clip(int x, int y, int w, int h)
{
    g_clip_on = 1;
    g_cx = x; g_cy = y; g_cw = w; g_ch = h;
}

void aether_render_clear_clip(void) { g_clip_on = 0; }

static inline int clipped(int x, int y)
{
    if (!g_clip_on) return 0;
    return (x < g_cx || y < g_cy || x >= g_cx + g_cw || y >= g_cy + g_ch);
}

void aether_render_init(unsigned char *bits, int w, int h, int pitch)
{
    g.bits = bits;
    g.w = w;
    g.h = h;
    g.pitch = pitch;
    g.cursor_x = w / 2;
    g.cursor_y = h / 2;
}

void aether_render_set_target(unsigned char *bits) { g.bits = bits; }

void aether_render_framebuffer(unsigned char **bits, int *w, int *h, int *pitch)
{
    if (bits)  *bits = g.bits;
    if (w)     *w = g.w;
    if (h)     *h = g.h;
    if (pitch) *pitch = g.pitch;
}

void aether_render_set_cursor(int x, int y)
{
    if (x >= 0 && x < g.w) g.cursor_x = x;
    if (y >= 0 && y < g.h) g.cursor_y = y;
}

int aether_render_cursor_x(void) { return g.cursor_x; }
int aether_render_cursor_y(void) { return g.cursor_y; }

static inline void set_px(int x, int y, unsigned rgb)
{
    int px = x + g_tx, py = y + g_ty;
    if (px < 0 || py < 0 || px >= g.w || py >= g.h) return;
    if (clipped(x, y)) return;
    unsigned *p = (unsigned *)(g.bits + (size_t)py * g.pitch);
    if (g_alpha >= 0xFF) { p[px] = rgb; return; }
    if (g_alpha == 0) return;
    unsigned char r = (rgb >> 16) & 0xFF, g8 = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    unsigned cur = p[px];
    unsigned char cr = (cur >> 16) & 0xFF, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
    p[px] = 0xFF000000u |
        ((unsigned)((r * g_alpha + cr * (255 - g_alpha)) / 255) << 16) |
        ((unsigned)((g8 * g_alpha + cg * (255 - g_alpha)) / 255) << 8) |
        (unsigned)((b * g_alpha + cb * (255 - g_alpha)) / 255);
}

static inline unsigned get_px(int x, int y)
{
    int px = x + g_tx, py = y + g_ty;
    if (px < 0 || py < 0 || px >= g.w || py >= g.h || clipped(x, y)) return 0;
    unsigned *p = (unsigned *)(g.bits + (size_t)py * g.pitch);
    return p[px];
}

void aether_clear(unsigned rgb)
{
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++) set_px(x, y, rgb);
}

void aether_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) set_px(i, j, rgb);
}

void aether_fill_rect_alpha(int x, int y, int w, int h, unsigned rgb, unsigned char a)
{
    unsigned char r = (rgb >> 16) & 0xFF, g8 = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            unsigned cur = get_px(i, j);
            unsigned char cr = (cur >> 16) & 0xFF, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
            set_px(i, j, pack_rgb(
                (unsigned char)((r * a + cr * (255 - a)) / 255),
                (unsigned char)((g8 * a + cg * (255 - a)) / 255),
                (unsigned char)((b * a + cb * (255 - a)) / 255)));
        }
    }
}

void aether_blend_pixel(int x, int y, unsigned rgb, unsigned char a)
{
    if (a == 0) return;
    unsigned char r = (rgb >> 16) & 0xFF, g8 = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (a == 0xFF) { set_px(x, y, rgb); return; }
    unsigned cur = get_px(x, y);
    unsigned char cr = (cur >> 16) & 0xFF, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
    set_px(x, y, pack_rgb(
        (unsigned char)((r * a + cr * (255 - a)) / 255),
        (unsigned char)((g8 * a + cg * (255 - a)) / 255),
        (unsigned char)((b * a + cb * (255 - a)) / 255)));
}

void aether_draw_border(int x, int y, int w, int h, unsigned rgb, int t)
{
    aether_fill_rect(x, y, w, t, rgb);
    aether_fill_rect(x, y + h - t, w, t, rgb);
    aether_fill_rect(x, y, t, h, rgb);
    aether_fill_rect(x + w - t, y, t, h, rgb);
}

void aether_draw_vgradient(int x, int y, int w, int h, unsigned top, unsigned bot)
{
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        int t = (j * 100) / h;
        unsigned char r = (unsigned char)((((top >> 16) & 0xFF) * (100 - t) + ((bot >> 16) & 0xFF) * t) / 100);
        unsigned char gg = (unsigned char)((((top >> 8) & 0xFF) * (100 - t) + ((bot >> 8) & 0xFF) * t) / 100);
        unsigned char b = (unsigned char)(((top & 0xFF) * (100 - t) + (bot & 0xFF) * t) / 100);
        unsigned c = pack_rgb(r, gg, b);
        for (int i = 0; i < w; i++) set_px(x + i, y + j, c);
    }
}

static int in_round_rect(int px, int py, int x, int y, int w, int h, int r)
{
    int cx = px - x, cy = py - y;
    if (cx < 0 || cy < 0 || cx >= w || cy >= h) return 0;
    if (r <= 0) return 1;
    if (cx < r && cy < r) { int dx = r - cx, dy = r - cy; if (dx*dx + dy*dy > r*r) return 0; }
    else if (cx >= w - r && cy < r) { int dx = cx - (w - r), dy = r - cy; if (dx*dx + dy*dy > r*r) return 0; }
    else if (cx < r && cy >= h - r) { int dx = r - cx, dy = cy - (h - r); if (dx*dx + dy*dy > r*r) return 0; }
    else if (cx >= w - r && cy >= h - r) { int dx = cx - (w - r), dy = cy - (h - r); if (dx*dx + dy*dy > r*r) return 0; }
    return 1;
}

void aether_draw_round_rect(int x, int y, int w, int h, int r, unsigned rgb, unsigned char alpha)
{
    unsigned char rr = (rgb >> 16) & 0xFF, gg = (rgb >> 8) & 0xFF, bb = rgb & 0xFF;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (!in_round_rect(i, j, x, y, w, h, r)) continue;
            if (alpha == 0xFF) { set_px(i, j, rgb); continue; }
            unsigned cur = get_px(i, j);
            unsigned char cr = (cur >> 16) & 0xFF, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
            set_px(i, j, pack_rgb(
                (unsigned char)((rr * alpha + cr * (255 - alpha)) / 255),
                (unsigned char)((gg * alpha + cg * (255 - alpha)) / 255),
                (unsigned char)((bb * alpha + cb * (255 - alpha)) / 255)));
        }
    }
}

/* ---- UTF-8 解码 ---- */
static int utf8_next(const char **p, unsigned *cp)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!s[0]) return 0;
    if (s[0] < 0x80) { *cp = s[0]; *p += 1; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        *cp = ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p += 2; return 1;
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((unsigned)(s[0] & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3; return 1;
    }
    *cp = '?'; *p += 1; return 1;
}

int aether_text_width(const char *s) { return (int)strlen(s) * (int)AETHER_FONT_W; }

int aether_text_width_utf8(const char *s)
{
    int w = 0;
    unsigned cp;
    const char *p = s;
    while (utf8_next(&p, &cp)) {
        if (cp >= 0x80 && aether_cjk_find(cp) >= 0)
            w += AETHER_CJK_W;
        else
            w += AETHER_FONT_W;
    }
    return w;
}

static void draw_ascii(int x, int y, unsigned char c, unsigned rgb)
{
    if (c < 0x20 || c > 0x7E) c = 0x20;
    const unsigned char *gl = aether_font_8x16[c - 0x20];
    for (int row = 0; row < (int)AETHER_FONT_H; row++) {
        unsigned char bits = gl[row];
        for (int col = 0; col < (int)AETHER_FONT_W; col++)
            if (bits & (0x80 >> col)) set_px(x + col, y + row, rgb);
    }
}

static void draw_cjk(int x, int y, unsigned cp, unsigned rgb)
{
    int idx = aether_cjk_find(cp);
    if (idx < 0) { draw_ascii(x + 4, y, '?', rgb); return; }
    const unsigned char *gl = aether_cjk_glyph[idx];
    for (int row = 0; row < (int)AETHER_CJK_H; row++) {
        unsigned short bits = (unsigned short)(((unsigned)gl[row * 2] << 8) | gl[row * 2 + 1]);
        for (int col = 0; col < (int)AETHER_CJK_W; col++)
            if (bits & (0x8000u >> col)) set_px(x + col, y + row, rgb);
    }
}

void aether_draw_text(int x, int y, const char *s, unsigned rgb)
{
    int cx = x;
    for (int k = 0; s[k]; k++) { draw_ascii(cx, y, (unsigned char)s[k], rgb); cx += AETHER_FONT_W; }
}

void aether_draw_text_utf8(int x, int y, const char *s, unsigned rgb)
{
    int cx = x;
    unsigned cp;
    const char *p = s;
    while (utf8_next(&p, &cp)) {
        if (cp >= 0x80 && aether_cjk_find(cp) >= 0) {
            draw_cjk(cx, y, cp, rgb);
            cx += AETHER_CJK_W;
        } else {
            draw_ascii(cx, y, (unsigned char)(cp < 0x80 ? cp : '?'), rgb);
            cx += AETHER_FONT_W;
        }
    }
}

/* ASCII 文字按目标像素高 h 做最近邻缩放（用于锁屏大时钟等） */
void aether_draw_text_scale(const char *s, int x, int y, int h, unsigned rgb)
{
    if (h <= 0) return;
    int scale = (h + AETHER_FONT_H - 1) / AETHER_FONT_H;
    if (scale < 1) scale = 1;
    int sw = AETHER_FONT_W * scale;
    int cx = x;
    for (int k = 0; s[k]; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c > 0x7E) c = 0x20;
        const unsigned char *gl = aether_font_8x16[c - 0x20];
        for (int row = 0; row < (int)AETHER_FONT_H; row++) {
            unsigned char bits = gl[row];
            for (int col = 0; col < (int)AETHER_FONT_W; col++) {
                if (bits & (0x80 >> col))
                    aether_fill_rect(cx + col * scale, y + row * scale, scale, scale, rgb);
            }
        }
        cx += sw;
    }
}

/* ---- 系统图标（Tux，内嵌位图，来源 gilbarbara/logos） ---- */
void aether_draw_logo(int cx, int cy, int size)
{
    const int W = AETHER_LOGO_W, H = AETHER_LOGO_H;
    const int x0 = cx - W / 2, y0 = cy - H / 2;
    (void)size;
    for (int py = 0; py < H; py++) {
        int dy = y0 + py;
        if (dy < 0 || dy >= g.h) continue;
        for (int px = 0; px < W; px++) {
            int dx = x0 + px;
            if (dx < 0 || dx >= g.w) continue;
            unsigned v = aether_logo_px[(size_t)py * W + px];
            unsigned char a = (unsigned char)((v >> 24) & 0xFF);
            if (a == 0) continue;
            unsigned char r = (v >> 16) & 0xFF, g8 = (v >> 8) & 0xFF, b = v & 0xFF;
            if (a == 0xFF) { set_px(dx, dy, pack_rgb(r, g8, b)); continue; }
            unsigned cur = get_px(dx, dy);
            unsigned char cr = (cur >> 16) & 0xFF, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
            set_px(dx, dy, pack_rgb(
                (unsigned char)((r * a + cr * (255 - a)) / 255),
                (unsigned char)((g8 * a + cg * (255 - a)) / 255),
                (unsigned char)((b * a + cb * (255 - a)) / 255)));
        }
    }
}

static const unsigned short CURSOR[16] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xEFF0, 0xFFF8, 0xBFF8, 0x7FE0, 0x3F80, 0x00E0,
};

void aether_draw_cursor(int x, int y)
{
    for (int row = 0; row < 16; row++) {
        unsigned short bits = CURSOR[row];
        for (int col = 0; col < 16; col++) {
            if (bits & (0x8000 >> col)) {
                set_px(x + col, y + row, pack_rgb(20, 20, 24));
                set_px(x + col + 1, y + row, pack_rgb(255, 255, 255));
            }
        }
    }
}

/* ==================== 画布快照 / 位块传送 ====================
 * 供转场视差使用；逐像素读写遵守边界。真算模糊与液态玻璃材质
 * 已独立到 aether-glass.c（开源实现照搬），此处不再重复实现。 */

static unsigned fx_sample_px(int x, int y)
{
    if (x < 0 || y < 0 || x >= g.w || y >= g.h) return 0;
    unsigned *p = (unsigned *)(g.bits + (size_t)y * g.pitch);
    return p[x];
}

/* 真算实时模糊与液态玻璃材质已迁移到 aether-glass.c。
 * 该实现照搬自 Kyant0/AndroidLiquidGlass (Apache-2.0) 与
 * sdegenaar/liquid_glass_widgets (MIT) 的光学公式/参数/降级规则，
 * 以 C 软渲染等价移植；旧的自研 box blur（放大阶段定点插值导致彩色条纹）
 * 已移除。此处保留薄转发，维持既有调用点不变。 */
void aether_blur_rect(int x, int y, int w, int h, int radius)
{
    aether_glass_blur_rect(x, y, w, h, radius);
}

void aether_blur_tint(int x, int y, int w, int h, int radius, unsigned tint, unsigned char a)
{
    aether_glass_blur_tint(x, y, w, h, radius, tint, a);
}

unsigned *aether_capture_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    unsigned *buf = (unsigned *)malloc((size_t)w * h * 4);
    if (!buf) return NULL;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int sx = x + i, sy = y + j;
        unsigned v = (sx >= 0 && sy >= 0 && sx < g.w && sy < g.h) ? fx_sample_px(sx, sy) : 0;
        buf[j * w + i] = v;
    }
    return buf;
}

void aether_blit_rect(unsigned *buf, int x, int y, int w, int h, unsigned char a)
{
    if (!buf) return;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        if (a == 0xFF) { set_px(x + i, y + j, buf[j * w + i]); continue; }
        unsigned src = buf[j * w + i];
        unsigned char sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
        unsigned cur = get_px(x + i, y + j);
        unsigned char cr = (cur >> 16) & 0xFF, cg = (cur >> 8) & 0xFF, cb = cur & 0xFF;
        set_px(x + i, y + j, 0xFF000000u |
            ((unsigned)((sr * a + cr * (255 - a)) / 255) << 16) |
            ((unsigned)((sg * a + cg * (255 - a)) / 255) << 8) |
            (unsigned)((sb * a + cb * (255 - a)) / 255));
    }
}

void aether_free_capture(unsigned *buf) { free(buf); }

/* ---- 旧版静态欢迎页（保留兼容，新桌面不再使用） ---- */
void aether_render_desktop(const char *clock, double cpu, double mem, int win)
{
    aether_draw_vgradient(0, 0, g.w, g.h, pack_rgb(12, 18, 34), pack_rgb(8, 34, 48));
    aether_fill_rect_alpha(0, 0, g.w, 30, 0x000000, 150);
    aether_draw_text(12, 7, "AetherOS", pack_rgb(255, 255, 255));
    char right[64];
    snprintf(right, sizeof right, "CPU %4.1f%%  MEM %4.1f%%  %s", cpu, mem, clock ? clock : "");
    aether_draw_text(g.w - (int)strlen(right) * AETHER_FONT_W - 12, 7, right, pack_rgb(200, 230, 255));
    int cx = g.w / 2, cy = g.h / 2 - 30;
    aether_draw_logo(cx, cy, 150);
    const char *name = "AetherOS";
    aether_draw_text(cx - (int)strlen(name) * AETHER_FONT_W / 2, cy + 95, name, pack_rgb(235, 245, 255));
    const char *sub = "x86_64 Desktop Operating System";
    aether_draw_text(cx - (int)strlen(sub) * AETHER_FONT_W / 2, cy + 118, sub, pack_rgb(150, 190, 220));
    (void)win;
    aether_draw_cursor(g.cursor_x, g.cursor_y);
}
