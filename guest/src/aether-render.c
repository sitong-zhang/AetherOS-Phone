/*
 * aether-render.c - AetherOS 渲染层实现
 *
 * 逐像素软件光栅化。文字有两种后端（构建期由 AETHER_VECTOR_FONT 选择）：
 *   1. 矢量（默认）：Noto Sans CJK SC 的 TrueType 子集 + stb_truetype 抗锯齿
 *      栅格化，字形按【基线】定位进 16px 行盒，覆盖全 CJK 且任意尺寸平滑；
 *   2. 点阵（回退）：DejaVu Sans Mono 8x16 / CJK 16x16 点阵，仅 1001 字。
 * 两种后端共用同一套字符步进（ASCII 8px / 非 ASCII 16px）与 text_width，
 * 因此切换后端不改变任何既有界面布局。
 */
#include "aether-render.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AETHER_VECTOR_FONT
/* 矢量后端：布局度量仍沿用 8x16 / 16x16 的格子，保证界面零改动 */
#include "aether-vfont.h"
#define AETHER_FONT_W 8u
#define AETHER_FONT_H 16u
#define AETHER_CJK_W 16u
#define AETHER_CJK_H 16u
#else
/* 点阵回退后端 */
#include "aether-font.h"
#include "aether-font-cjk.h"
#endif
#include "aether-logo.h"
#include "aether-glass.h"
#include "aether-tokens.h"

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
    /* 仅记录最后触点坐标（EV_ABS 分轴上报时累积用），不绘制任何指针 */
    g.touch_x = w / 2;
    g.touch_y = h / 2;
}

void aether_render_set_target(unsigned char *bits) { g.bits = bits; }

void aether_render_framebuffer(unsigned char **bits, int *w, int *h, int *pitch)
{
    if (bits)  *bits = g.bits;
    if (w)     *w = g.w;
    if (h)     *h = g.h;
    if (pitch) *pitch = g.pitch;
}

void aether_render_set_touch(int x, int y)
{
    if (x >= 0 && x < g.w) g.touch_x = x;
    if (y >= 0 && y < g.h) g.touch_y = y;
}

int aether_render_touch_x(void) { return g.touch_x; }
int aether_render_touch_y(void) { return g.touch_y; }

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

/* ---- 文本度量 ----
 * 字符步进固定为 ASCII 8px / 非 ASCII 16px，与旧点阵后端一致。界面里所有
 * 居中、换行、文本容器计算都依赖这两个值，因此切换字体后端不改变布局。 */
static int text_cell_w(unsigned cp)
{
    return cp >= 0x80 ? (int)AETHER_CJK_W : (int)AETHER_FONT_W;
}

int aether_text_width(const char *s) { return (int)strlen(s) * (int)AETHER_FONT_W; }

int aether_text_width_utf8(const char *s)
{
    int w = 0;
    unsigned cp;
    const char *p = s;
    while (utf8_next(&p, &cp))
        w += text_cell_w(cp);
    return w;
}

#ifdef AETHER_VECTOR_FONT

/* 把矢量字形按【基线】放进 line_h 高的行盒，并水平居中于 cell_w 内。
 *
 * 这是「文字底部裁切」的根因修复：旧点阵把参数 y 当作字形第一行、直接硬画
 * 16 行，一旦文本容器按 ascent 计算高度，字形下缘/descender 就会被裁。
 * 矢量后端用 stbtt 的 y0 与 asc_px 换算基线（ScaleForPixelHeight 保证
 * ascent+descent 恰等于行盒高），字形完整落在 [y, y+line_h) 内。
 * 抗锯齿灰度通过 aether_blend_pixel 混合，自动遵守裁剪与全局淡入 alpha。 */
static void vfont_cell(int x, int y, unsigned cp, int cell_w, int line_h, unsigned rgb)
{
    const struct aether_glyph *g = aether_vfont_get(cp, line_h);
    if (!g) g = aether_vfont_get('?', line_h);       /* 缺字兜底 */
    if (!g || g->w <= 0 || g->h <= 0) return;         /* 空格等无轮廓 */
    int px = x + (cell_w - g->w) / 2;                 /* 格子内水平居中 */
    int py = y + g->asc_px + g->y0;                   /* 基线换算，不裁切 */
    for (int row = 0; row < g->h; row++) {
        for (int col = 0; col < g->w; col++) {
            unsigned char a = g->alpha[(size_t)row * g->w + col];
            if (a) aether_blend_pixel(px + col, py + row, rgb, a);
        }
    }
}

void aether_draw_text(int x, int y, const char *s, unsigned rgb)
{
    int cx = x;
    for (int k = 0; s[k]; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c > 0x7E) c = 0x20;
        vfont_cell(cx, y, c, (int)AETHER_FONT_W, (int)AETHER_FONT_H, rgb);
        cx += (int)AETHER_FONT_W;
    }
}

void aether_draw_text_utf8(int x, int y, const char *s, unsigned rgb)
{
    int cx = x;
    unsigned cp;
    const char *p = s;
    while (utf8_next(&p, &cp)) {
        int cw = text_cell_w(cp);
        vfont_cell(cx, y, cp, cw, (int)AETHER_FONT_H, rgb);
        cx += cw;
    }
}

/* 按目标像素高缩放：字符步进沿用 8*ceil(h/16)、行盒 16*ceil(h/16)，
 * 布局与旧实现完全一致；字形改为矢量抗锯齿渲染，不再最近邻放大。 */
void aether_draw_text_scale(const char *s, int x, int y, int h, unsigned rgb)
{
    if (h <= 0) return;
    int scale = (h + (int)AETHER_FONT_H - 1) / (int)AETHER_FONT_H;
    if (scale < 1) scale = 1;
    int sw = (int)AETHER_FONT_W * scale;
    int line_h = (int)AETHER_FONT_H * scale;
    int cx = x;
    for (int k = 0; s[k]; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c > 0x7E) c = 0x20;
        vfont_cell(cx, y, c, sw, line_h, rgb);
        cx += sw;
    }
}

#else /* ---- 点阵回退后端：无矢量字体资产时仍可构建（仅收录 1001 字）---- */

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
        int cw = text_cell_w(cp);
        if (cp >= 0x80) {
            draw_cjk(cx, y, cp, rgb);
        } else {
            draw_ascii(cx, y, (unsigned char)cp, rgb);
        }
        cx += cw;
    }
}

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

#endif /* AETHER_VECTOR_FONT */

/* 与 aether_draw_text_scale() 完全同公式的缩放后宽度（两个字体后端共用）。
 * draw_text_scale 把每个 ASCII 字符放大为 scale=ceil(h/FONT_H) 倍、
 * 步进 FONT_W*scale 像素；旧代码却用未缩放的 aether_text_width_utf8()
 * 去居中，导致大字号字标右缘溢出屏幕被裁（开机动画 "Aether OS" 即此因）。 */
int aether_text_width_scale(const char *s, int h)
{
    if (h <= 0 || !s) return 0;
    int scale = (h + (int)AETHER_FONT_H - 1) / (int)AETHER_FONT_H;
    if (scale < 1) scale = 1;
    return (int)strlen(s) * (int)AETHER_FONT_W * scale;
}

/* ==================== 字号感知排版（分册 §5.2 / §5.3 / §5.5） ====================
 * 旧接口固定 8x16 行盒，业务层曾写死「(h - 16) / 2」做垂直居中；矢量字体
 * 真实 ascent/descent 之和并不等于 16px，descender（g / p / y / 竖 / 鄴）
 * 会落到行盒外被裁。这里提供按真实字体度量计算基线的统一入口：
 *
 *   line_h  = max(ceil(size * 1.5), ascent + descent + 4)
 *   baseline = y + (line_h - (ascent + descent)) / 2 + ascent
 *
 * 字形位图左上角 = (x + (cell_w - w) / 2, baseline + y0)，y0 为 stbtt
 * 相对基线的偏移（负值）。缺字回落 U+25A1 □，不静默留白。
 * 字符步进沿用「ASCII 半宽 / 其他全宽」的实调格子：size=16 时恰为 8/16，
 * 与既有 8x16 布局完全一致，因此新增字号不会改动任何既有界面坐标。 */
static int sized_cell_w(unsigned cp, int size)
{
    int half = size / 2;
    if (half < 1) half = 1;
    return cp >= 0x80 ? size : half;   /* 全角方块 / 半角拉丁 */
}

int aether_text_width_utf8_sized(const char *s, int size)
{
    if (size < 1) size = TK_T_COMPAT;
    int w = 0;
    unsigned cp;
    const char *p = s;
    while (utf8_next(&p, &cp)) w += sized_cell_w(cp, size);
    return w;
}

int aether_text_ascent(int size)
{
    if (size < 1) size = TK_T_COMPAT;
    int a = 0, d = 0;
#ifdef AETHER_VECTOR_FONT
    if (aether_vfont_metrics(size, &a, &d)) return a;
#endif
    a = size * 4 / 5;  /* 点阵回退：8x16 点阵的近似度量 */
    return a > 0 ? a : 1;
}

int aether_text_descent(int size)
{
    if (size < 1) size = TK_T_COMPAT;
    int a = 0, d = 0;
#ifdef AETHER_VECTOR_FONT
    if (aether_vfont_metrics(size, &a, &d)) return d;
#endif
    d = size - (size * 4 / 5);
    return d >= 0 ? d : 0;
}

int aether_text_line_h(int size)
{
    if (size < 1) size = TK_T_COMPAT;
    int lh = TK_LINE_H(size);
    int need = aether_text_ascent(size) + aether_text_descent(size) + 4;
    if (lh < need) lh = need;          /* 上下各 2px 光栅余量，不得裁切 */
    return lh;
}

int aether_text_box_y(int box_y, int box_h, int size)
{
    int lh = aether_text_line_h(size);
    if (box_h <= lh) return box_y;     /* 容器不足时保顶不保底 */
    return box_y + (box_h - lh) / 2;
}

static void draw_cell_sized(int x, int y, unsigned cp, int cell_w,
                            int size, int line_h, unsigned rgb)
{
#ifdef AETHER_VECTOR_FONT
    const struct aether_glyph *gl = aether_vfont_get(cp, size);
    if (!gl) gl = aether_vfont_get(0x25A1, size);   /* □ 缺字回落 */
    if (!gl || gl->w <= 0 || gl->h <= 0) return;
    int asc = gl->asc_px;
    int desc = aether_text_descent(size);
    int baseline = y + (line_h - (asc + desc)) / 2 + asc;
    int py = baseline + gl->y0;
    int px = x + (cell_w - gl->w) / 2;
    for (int r = 0; r < gl->h; r++) {
        for (int c = 0; c < gl->w; c++) {
            unsigned char a = gl->alpha[(size_t)r * gl->w + c];
            if (a) aether_blend_pixel(px + c, py + r, rgb, a);
        }
    }
#else
    /* 点阵回退后端只有 8x16 / 16x16 固定字形，不支持任意字号放大；
     * 按真实行盒居中绘制 16px 字形，保证文字完整可见、不裁切。 */
    int gy = y + (line_h - (int)AETHER_FONT_H) / 2;
    if (gy < y) gy = y;
    if (cp >= 0x80) draw_cjk(x, gy, cp, rgb);
    else            draw_ascii(x, gy, (unsigned char)cp, rgb);
#endif
}

void aether_draw_text_utf8_sized(int x, int y, const char *s, int size, unsigned rgb)
{
    if (size < 1) size = TK_T_COMPAT;
    int lh = aether_text_line_h(size);
    int cx = x;
    unsigned cp;
    const char *p = s;
    while (utf8_next(&p, &cp)) {
        int cw = sized_cell_w(cp, size);
        draw_cell_sized(cx, y, cp, cw, size, lh, rgb);
        cx += cw;
    }
}

/* 单行截断（分册 §5.5）：按 UTF-8 字符粒度累加，禁止按字节截断产生半个汉字；
 * 尾部省略号 U+2026 本身占 1em，需提前预留。禁止用 clip 代替省略号。 */
void aether_text_fit_sized(char *dst, size_t cap, const char *src, int size, int maxpx)
{
    static const char ELL[] = "\xE2\x80\xA6";   /* … */
    int ellw = sized_cell_w(0x2026, size);
    int budget = maxpx - ellw;
    if (budget < 0) budget = 0;
    size_t o = 0;
    int w = 0, truncated = 0;
    const char *p = src;
    while (*p) {
        const char *q = p;
        unsigned cp;
        if (!utf8_next(&q, &cp)) break;
        int n = (int)(q - p);
        int cw = sized_cell_w(cp, size);
        if (w + cw > maxpx || o + (size_t)n + 1 >= cap) { truncated = 1; break; }
        memcpy(dst + o, p, (size_t)n);
        o += (size_t)n; w += cw; p = q;
    }
    if (truncated) {
        o = 0; w = 0; p = src;
        while (*p) {
            const char *q = p;
            unsigned cp;
            if (!utf8_next(&q, &cp)) break;
            int n = (int)(q - p);
            int cw = sized_cell_w(cp, size);
            if (w + cw > budget || o + (size_t)n + sizeof(ELL) >= cap) break;
            memcpy(dst + o, p, (size_t)n);
            o += (size_t)n; w += cw; p = q;
        }
        memcpy(dst + o, ELL, sizeof(ELL) - 1);
        o += sizeof(ELL) - 1;
    }
    dst[o] = 0;
}

/* 盒内排版：水平居中 + 按真实字体度量垂直居中 + 尾部省略号。
 * 按钮、标题栏、状态栏等一律走此入口，业务层不再出现裸字号与固定 16。 */
void aether_draw_text_utf8_box(int x, int y, int w, int h,
                               const char *s, int size, unsigned rgb)
{
    if (w <= 0 || h <= 0) return;
    char buf[256];
    aether_text_fit_sized(buf, sizeof buf, s, size, w);
    int tw = aether_text_width_utf8_sized(buf, size);
    int tx = x + (w - tw) / 2;
    int ty = aether_text_box_y(y, h, size);
    aether_draw_text_utf8_sized(tx, ty, buf, size, rgb);
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

/* PC 方案已废弃：鼠标指针位图与对应的指针绘制函数整体移除。
 * 本系统只支持触摸输入与触控手势，界面内不出现任何指针。 */

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

/* 旧版「x86_64 Desktop」静态欢迎页已删除：该项目不存在 PC 版本，
 * 且该页曾是唯一的指针绘制点。手机壳由 aether-phone.c 提供。 */
