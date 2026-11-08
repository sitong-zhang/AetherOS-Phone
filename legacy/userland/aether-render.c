/*
 * aether-render.c - software rasterizer implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether-render.h"
#include "aether-font.h"
#include "aether-font-cjk.h"

/* Desktop Chinese status line (P1-1b). Mixed ASCII + CJK; the CJK codepoints
 * must all exist in aether-font-cjk.h (they do: 图形合成器窗口系统已就绪).
 * Location asserted by boot-smoke J16. */
static const char AETHER_DESKTOP_CN[] = "AetherOS 图形合成器 窗口系统已就绪";

void aether_render_init_fb(aether_fb_t *fb, void *map,
                           uint32_t w, uint32_t h, uint32_t pitch)
{
    fb->px = (uint32_t *)map;
    fb->w = w;
    fb->h = h;
    fb->pitch = pitch;
}

void aether_render_fill(aether_fb_t *fb, uint32_t rgb)
{
    uint32_t stride = fb->pitch / 4;
    for (uint32_t y = 0; y < fb->h; y++) {
        uint32_t *row = fb->px + (size_t)y * stride;
        for (uint32_t x = 0; x < fb->w; x++)
            row[x] = rgb;
    }
}

void aether_render_rect(aether_fb_t *fb, int x, int y, int w, int h,
                        uint32_t rgb)
{
    uint32_t stride = fb->pitch / 4;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->w) w = fb->w - x;
    if (y + h > (int)fb->h) h = fb->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t *row = fb->px + (size_t)yy * stride;
        for (int xx = x; xx < x + w; xx++)
            row[xx] = rgb;
    }
}

void aether_render_gradient_v(aether_fb_t *fb, uint32_t top, uint32_t bottom)
{
    uint32_t stride = fb->pitch / 4;
    int r0 = (top >> 16) & 0xFF, g0 = (top >> 8) & 0xFF, b0 = top & 0xFF;
    int r1 = (bottom >> 16) & 0xFF, g1 = (bottom >> 8) & 0xFF, b1 = bottom & 0xFF;

    for (uint32_t y = 0; y < fb->h; y++) {
        uint32_t t = fb->h > 1 ? (y * 256) / (fb->h - 1) : 0;
        uint32_t r = (r0 * (256 - (int)t) + r1 * (int)t) / 256;
        uint32_t g = (g0 * (256 - (int)t) + g1 * (int)t) / 256;
        uint32_t b = (b0 * (256 - (int)t) + b1 * (int)t) / 256;
        uint32_t rgb = (r << 16) | (g << 8) | b;
        uint32_t *row = fb->px + (size_t)y * stride;
        for (uint32_t x = 0; x < fb->w; x++)
            row[x] = rgb;
    }
}

void aether_render_text(aether_fb_t *fb, int x, int y, const char *s,
                        uint32_t rgb)
{
    uint32_t stride = fb->pitch / 4;

    while (s && *s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch < 0x20 || ch > 0x7E)
            ch = '?';
        const unsigned char *glyph = aether_font_8x16[ch - 0x20];
        for (int gy = 0; gy < 16; gy++) {
            int py = y + gy;
            if (py < 0 || py >= (int)fb->h)
                continue;
            uint32_t *row = fb->px + (size_t)py * stride;
            unsigned char bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                int px = x + gx;
                if (px < 0 || px >= (int)fb->w)
                    continue;
                if (bits & (0x80 >> gx))
                    row[px] = rgb;
            }
        }
        x += 8;
    }
}

/* ---- P1-1b: UTF-8 mixed text (ASCII 8x16 + CJK 16x16) ---------------- */

static int cjk_find(uint32_t cp)
{
    int lo = 0, hi = (int)AETHER_CJK_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (aether_cjk_cp[mid] == cp)
            return mid;
        else if (aether_cjk_cp[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void blit_glyph8(aether_fb_t *fb, int x, int y,
                        const unsigned char *g, uint32_t rgb)
{
    uint32_t stride = fb->pitch / 4;
    for (int gy = 0; gy < 16; gy++) {
        int py = y + gy;
        if (py < 0 || py >= (int)fb->h)
            continue;
        unsigned char bits = g[gy];
        uint32_t *row = fb->px + (size_t)py * stride;
        for (int gx = 0; gx < 8; gx++) {
            int px = x + gx;
            if (px < 0 || px >= (int)fb->w)
                continue;
            if (bits & (0x80u >> gx))
                row[px] = rgb;
        }
    }
}

static void blit_glyph16(aether_fb_t *fb, int x, int y,
                         const uint8_t *g, uint32_t rgb)
{
    uint32_t stride = fb->pitch / 4;
    for (int gy = 0; gy < 16; gy++) {
        int py = y + gy;
        if (py < 0 || py >= (int)fb->h)
            continue;
        uint32_t row16 = ((uint32_t)g[2 * gy] << 8) | g[2 * gy + 1];
        uint32_t *row = fb->px + (size_t)py * stride;
        for (int gx = 0; gx < 16; gx++) {
            int px = x + gx;
            if (px < 0 || px >= (int)fb->w)
                continue;
            if (row16 & (1u << (15 - gx)))
                row[px] = rgb;
        }
    }
}

void aether_render_text_utf8(aether_fb_t *fb, int x, int y,
                             const char *s, uint32_t rgb)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        uint32_t cp;
        int adv;
        unsigned char c = *p;
        if (c < 0x80) {                 /* ASCII */
            cp = c; p++; adv = 8;
        } else if ((c & 0xE0) == 0xC0 && p[1]) {        /* 2-byte */
            cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2; adv = 16;
        } else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { /* 3-byte (BMP) */
            cp = ((uint32_t)(c & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3; adv = 16;
        } else {                        /* invalid lead byte: skip */
            p++;
            continue;
        }
        if (cp <= 0x7F) {
            if (cp >= 0x20)            /* skip control chars, still advance */
                blit_glyph8(fb, x, y, aether_font_8x16[cp - 0x20], rgb);
        } else {
            int idx = cjk_find(cp);
            if (idx >= 0)
                blit_glyph16(fb, x, y, aether_cjk_glyphs[idx], rgb);
            else {                      /* missing glyph: tofu outline */
                aether_render_rect(fb, x, y, 16, 1, rgb);
                aether_render_rect(fb, x, y + 15, 16, 1, rgb);
                aether_render_rect(fb, x, y, 1, 16, rgb);
                aether_render_rect(fb, x + 15, y, 1, 16, rgb);
            }
        }
        x += adv;
    }
}

int aether_text_width_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    int w = 0;
    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) { w += 8; p++; }
        else if ((c & 0xE0) == 0xC0 && p[1]) { w += 16; p += 2; }
        else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { w += 16; p += 3; }
        else { p++; }
    }
    return w;
}

/* ------------------------------------------------------------------ *
 * P4-1 液态玻璃底层 —— Kyant0/AndroidLiquidGlass（Apple Backdrop 2.0.0）
 * 上游配方的 CPU 软件光栅化移植。参数逐字取自上游配方表
 * （apple-liquid-glass-guide/docs/04-Material-Recipe-Table.md，注明
 * "All values are copied verbatim from the upstream"），不凭口味调整；
 * 唯一的环境适配是 dp -> px 的密度换算（上游警告：漏掉 .dp.toPx()
 * 会让折射只剩 1/3——我们必须显式声明密度）。
 *
 * 移植的数学（docs/09 §4.2 折射 shader verbatim）：
 *   circleMap(x)  = 1 - sqrt(1 - x^2)                    // 四分圆 -> 透镜剖面
 *   d             = circleMap(1 - (-sd)/H) * A           // 边缘最大、向内衰减
 *   grad          = normalize(gradSdRoundedRect + depth*normalize(centered))
 *   refracted     = coord + d * grad                     // 沿法线推/拉背景采样
 *   dispersion    = CA * (|x|*|y|)/(hw*hh)               // 角部最强，静止为 0
 *
 * 性能（TCG 关键）：深内部走无平方根快路；折射数学只在离边缘 H px 的
 * 带内计算；背景模糊在 4 倍降采样缓冲上做 box blur；色差（3 带近似上游
 * 7 光谱带）仅在 press>0 的交互窗口内启用——与上游"静止不色差"一致。
 * ------------------------------------------------------------------ */
#define GLASS_DS      4               /* backdrop downsample factor            */
/* 全屏锁屏/启动器面板的像素上界：必须覆盖手机竖屏形态 720x1600（1.152M）。
 * 曾是 1280x800——竖屏超限会静默降级为「flat tint」纯色块（整屏被刷成
 * 面板染色），竖屏冒烟 boot-smoke-portrait 抓到的就是这个。 */
#define GLASS_MAXPX   (1280u * 1600u)

/* dp -> px。上游配方是 dp 口径（实际传 .dp.toPx()）。取 2.0（2x 级平板/
 * 手机密度）显式声明：这是本软件光栅化器的屏幕密度，不是隐式假设。 */
#define AETHER_GLASS_DENSITY 2.0f
static inline float glass_dp(float dp) { return dp * AETHER_GLASS_DENSITY; }

/* ---- 上游配方参数（verbatim，GlassMaterials.kt） -------------------
 * NavigationBar = { vibrancy(); blur(4.dp); lens(16.dp, 32.dp) }
 * BottomSheet   = { vibrancy(); blur(4.dp); lens(24.dp, 48.dp, depthEffect) }
 * 经验律（docs/01 §4.1）：refractionAmount ≈ 2 x refractionHeight。      */
#define WIN_BLUR_PASSES  2              /* blur(4dp) ≈ 8px：4x 降采样上 2 遍 box blur r=2 */
#define WIN_LENS_H       glass_dp(16)   /* 32 px 折射带高度 */
#define WIN_LENS_A       glass_dp(32)   /* 64 px 最大位移 */
#define SHEET_BLUR_PASSES 2
#define SHEET_LENS_H     glass_dp(24)   /* 48 px */
#define SHEET_LENS_A     glass_dp(48)   /* 96 px */
#define SHEET_DEPTH      0.30f          /* depthEffect：球状凸起强度（dialog/sheet 专用） */

/* Highlight.Default = width 0.5dp / blurRadius w/2 / alpha 1.0 /
 * White@50% / angle 45° / falloff 1 / additive（docs/09 §5）。 */
#define HL_ANGLE_COS     0.70710678f    /* cos(45°) */
#define HL_ANGLE_SIN     0.70710678f    /* sin(45°) */
#define HL_ALPHA         0.5f           /* White @ 50% */

/* Shadow.Default = radius 24dp / offset (0, r/6) / Black 10%（docs/09 §6.1）。 */
#define SHADOW_R         glass_dp(24)   /* 48 px */
#define SHADOW_DY        (SHADOW_R / 6.0f)   /* 8 px，主要落在下方 */
#define SHADOW_A         0.10f

/* InnerShadow(radius 8dp)：内侧暗环 = 玻璃的"厚度"（docs/09 §6.1：
 * "亮边 + 内暗边 = 有厚度的实体"）。上游仅在按压态启用，这里取静态极弱值。 */
#define INNER_R          glass_dp(8)    /* 16 px */
#define INNER_A          0.30f

/* vibrancy()：Gaussian 模糊是低通滤波，会把饱和度打灰；上游用
 * 饱和度 1.5x + 轻微提亮补偿（docs/01 §5）。 */
#define VIBRANCY_SAT     1.5f
#define VIBRANCY_BRIGHT  2              /* +2/255 */

/* 自着色：Apple 区间 5%~35%（docs/01 §8 六线索表），取 14%。 */
#define TINT_MIX         0.14f

/* AetherOS 附加的顶部辉光（非上游参数）：玻璃厚度暗示，刻意保持微弱，
 * 避免抢走上游 Highlight.Default 的方向性边缘光。 */
#define SHEEN_A          0.10f

#define GLASS_SMAX (GLASS_MAXPX / 16 + 8192)   /* 降采样缓冲上界（含取整余量） */

static uint32_t glass_snap[GLASS_MAXPX];   /* backdrop snapshot (window-sized)  */
static uint32_t glass_small[GLASS_SMAX];   /* downsampled + blurred             */
static uint32_t glass_tmp[GLASS_SMAX];     /* box-blur scratch                  */

#define glass_clamp(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

/* rounded-box signed distance; dx,dy relative to centre (negative inside).
 * 与上游 sdRoundedRect 同式（docs/09 §4.1 verbatim）。 */
static inline float glass_sdf(float dx, float dy,
                              float hw, float hh, float r)
{
    float qx = (dx < 0.0f ? -dx : dx) - hw + r;
    float qy = (dy < 0.0f ? -dy : dy) - hh + r;
    float mx = qx > qy ? qx : qy;
    float inside = mx < 0.0f ? mx : 0.0f;
    float ax = qx > 0.0f ? qx : 0.0f;
    float ay = qy > 0.0f ? qy : 0.0f;
    return inside + __builtin_sqrtf(ax * ax + ay * ay) - r;
}

/* circleMap: quarter-circle falloff -> "lens" profile (upstream verbatim). */
static inline float glass_circle_map(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return 1.0f - __builtin_sqrtf(1.0f - x * x);
}

/* bilinear sample of the downsampled backdrop */
static uint32_t glass_sample(int bw, int bh, float fx, float fy)
{
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > bw - 1) fx = (float)(bw - 1);
    if (fy > bh - 1) fy = (float)(bh - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < bw ? x0 + 1 : bw - 1;
    int y1 = y0 + 1 < bh ? y0 + 1 : bh - 1;
    float tx = fx - x0, ty = fy - y0;
    uint32_t c00 = glass_small[y0 * bw + x0], c10 = glass_small[y0 * bw + x1];
    uint32_t c01 = glass_small[y1 * bw + x0], c11 = glass_small[y1 * bw + x1];
    int r = (int)(((c00 >> 16 & 255) * (1 - tx) + (c10 >> 16 & 255) * tx) * (1 - ty)
                + ((c01 >> 16 & 255) * (1 - tx) + (c11 >> 16 & 255) * tx) * ty + 0.5f);
    int g = (int)(((c00 >> 8 & 255) * (1 - tx) + (c10 >> 8 & 255) * tx) * (1 - ty)
                + ((c01 >> 8 & 255) * (1 - tx) + (c11 >> 8 & 255) * tx) * ty + 0.5f);
    int b = (int)(((c00 & 255) * (1 - tx) + (c10 & 255) * tx) * (1 - ty)
                + ((c01 & 255) * (1 - tx) + (c11 & 255) * tx) * ty + 0.5f);
    return AETHER_RGB(glass_clamp(r), glass_clamp(g), glass_clamp(b));
}

/* separable box blur on glass_small (horizontal -> tmp, vertical -> small) */
static void glass_boxblur(int bw, int bh, int r)
{
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int k = -r; k <= r; k++) {
                int xx = i + k; if (xx < 0 || xx >= bw) continue;
                uint32_t p = glass_small[j * bw + xx];
                sr += p >> 16 & 255; sg += p >> 8 & 255; sb += p & 255; n++;
            }
            glass_tmp[j * bw + i] = AETHER_RGB(sr / n, sg / n, sb / n);
        }
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int k = -r; k <= r; k++) {
                int yy = j + k; if (yy < 0 || yy >= bh) continue;
                uint32_t p = glass_tmp[yy * bw + i];
                sr += p >> 16 & 255; sg += p >> 8 & 255; sb += p & 255; n++;
            }
            glass_small[j * bw + i] = AETHER_RGB(sr / n, sg / n, sb / n);
        }
}

/* Downsample-average + blur passes of the snapshot behind the panel.
 * blur(N passes of r=2 on a 4x downsample) ≈ upstream blur(4dp) @ density 2. */
static void glass_backdrop(int w, int h, int passes)
{
    int D = GLASS_DS;
    int dw = (w + D - 1) / D, dh = (h + D - 1) / D;
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int jj = 0; jj < D; jj++) {
                int sy = j * D + jj; if (sy >= h) break;
                for (int ii = 0; ii < D; ii++) {
                    int sx = i * D + ii; if (sx >= w) break;
                    uint32_t p = glass_snap[(size_t)sy * w + sx];
                    sr += p >> 16 & 255; sg += p >> 8 & 255; sb += p & 255; n++;
                }
            }
            if (n) { sr /= n; sg /= n; sb /= n; }
            glass_small[(size_t)j * dw + i] = AETHER_RGB(sr, sg, sb);
        }
    for (int p = 0; p < passes; p++)
        glass_boxblur(dw, dh, 2);
}

/* Outer shadow (upstream Shadow.Default): the panel's rounded rect shifted
 * down by SHADOW_DY, blurred with radius SHADOW_R, Black 10%; erased from
 * inside the shape (upstream's Clear-blend trick), so only the outer glow
 * remains. Painted onto the backdrop BEFORE the glass composite. */
static void glass_outer_shadow(aether_fb_t *fb, int x, int y, int w, int h,
                               float radius)
{
    uint32_t stride = fb->pitch / 4;
    float cx = w * 0.5f, cy = h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int x0 = x - (int)SHADOW_R - 2, x1 = x + w + (int)SHADOW_R + 2;
    int y0 = y - (int)SHADOW_R - 2, y1 = y + h + (int)SHADOW_R + (int)SHADOW_DY + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb->w) x1 = (int)fb->w;
    if (y1 > (int)fb->h) y1 = (int)fb->h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = fb->px + (size_t)j * stride;
        for (int i = x0; i < x1; i++) {
            /* distance to the shifted (down by SHADOW_DY) panel rect */
            float d = glass_sdf(i + 0.5f - (x + cx),
                                j + 0.5f - (y + cy + SHADOW_DY), hw, hh, radius);
            if (d <= 0.0f)
                continue;               /* inside the shape: erased */
            float a = 1.0f - d / SHADOW_R;
            if (a <= 0.0f)
                continue;               /* beyond the blur radius */
            a *= SHADOW_A;
            uint32_t p = row[i];
            int r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
            r -= (int)(r * a); g -= (int)(g * a); b -= (int)(b * a);
            row[i] = AETHER_RGB(r, g, b);
        }
    }
}

/* One glass panel. lens_h/lens_a: upstream lens(refractionHeight,
 * refractionAmount). depth: depthEffect strength (0 = off). */
static void glass_panel(aether_fb_t *fb, int x, int y, int w, int h,
                        uint32_t tint, float radius, float opacity,
                        float lens_h, float lens_a, float depth,
                        int blur_passes, float press, int fullbleed)
{
    if (w <= 4 || h <= 4)
        return;
    if ((uint32_t)w * (uint32_t)h > GLASS_MAXPX) {
        aether_render_rect(fb, x, y, w, h, tint);   /* too big: flat tint */
        return;
    }
    /* 全屏表面（锁屏/主屏/全屏应用）：屏幕上根本看不到玻璃的"边"——
     * 边缘折射/高光/内阴影/外阴影都是为「悬浮面板」设计的，硬套到
     * 全屏会在四周画出一圈折射边框（v5 实机截图里那圈紫框就是它）。
     * 按上游的材质划分：全屏用纯 backdrop blur + vibrancy + 染色。 */
    if (fullbleed) {
        radius = 0.0f;
        lens_h = 0.0f;
        lens_a = 0.0f;
        depth = 0.0f;
    }
    uint32_t stride = fb->pitch / 4;

    /* 1) snapshot the backdrop behind the panel */
    for (int j = 0; j < h; j++)
        memcpy(glass_snap + (size_t)j * w,
               fb->px + (size_t)(y + j) * stride + x,
               (size_t)w * sizeof(uint32_t));

    /* 2) downsample + blur (upstream blur(4dp)) */
    glass_backdrop(w, h, blur_passes);

    /* 3) outer shadow under the panel (Shadow.Default) —— 全屏无边，不画 */
    if (!fullbleed)
        glass_outer_shadow(fb, x, y, w, h, radius);

    /* 4) composite the glass */
    float cx = w * 0.5f, cy = h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    float band = radius < lens_h ? lens_h : radius;
    float ix = hw - radius - band, iy = hh - radius - band;
    int tr = tint >> 16 & 255, tg = tint >> 8 & 255, tb = tint & 255;
    /* chromaticAberration: upstream enables it only during interaction
     * (press/drag progress); at rest it is exactly 0. */
    float ca = press > 0.0f ? (press > 1.0f ? 1.0f : press) : 0.0f;
    float hl_alpha = HL_ALPHA * (1.0f + 0.6f * ca);  /* InteractiveHighlight */
    for (int j = 0; j < h; j++) {
        uint32_t *row = fb->px + (size_t)(y + j) * stride + x;
        for (int i = 0; i < w; i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            float ox = 0.0f, oy = 0.0f, cov = 1.0f;
            float hl = 0.0f, ish = 0.0f;
            int fast = 0;
            if (dx > -ix && dx < ix && dy > -iy && dy < iy) {
                fast = 1;    /* deep interior: -sd >= lens_h -> passthrough */
            } else {
                float sd = glass_sdf(dx, dy, hw, hh, radius);
                if (sd > 0.5f)
                    continue;                   /* rounded corner: outside */
                cov = sd < -0.5f ? 1.0f : (0.5f - sd);   /* edge AA */
                if (sd < 0.0f) {
                    /* ---- lens: upstream refraction shader (verbatim) ---- */
                    float d = glass_circle_map(1.0f - (-sd) / lens_h) * lens_a;
                    /* gradSdRoundedRect: outward normal of the nearest edge */
                    float qx = (dx < 0.0f ? -dx : dx) - (hw - radius);
                    float qy = (dy < 0.0f ? -dy : dy) - (hh - radius);
                    float gx, gy;
                    if (qx > 0.0f && qy > 0.0f) {
                        float l = __builtin_sqrtf(qx * qx + qy * qy);
                        if (l > 0.0f) {
                            gx = (dx < 0.0f ? -qx : qx) / l;
                            gy = (dy < 0.0f ? -qy : qy) / l;
                        } else { gx = 0.0f; gy = 0.0f; }
                    } else if (qx > 0.0f) {
                        gx = dx < 0.0f ? -1.0f : 1.0f; gy = 0.0f;
                    } else if (qy > 0.0f) {
                        gx = 0.0f; gy = dy < 0.0f ? -1.0f : 1.0f;
                    } else {
                        gx = 0.0f; gy = 0.0f;
                    }
                    /* depthEffect: spherical component so thick panels
                     * (dialogs / sheets) bulge in the middle too */
                    if (depth > 0.0f) {
                        float cl = __builtin_sqrtf(dx * dx + dy * dy);
                        if (cl > 0.001f) {
                            gx += depth * dx / cl;
                            gy += depth * dy / cl;
                        }
                        float gl = __builtin_sqrtf(gx * gx + gy * gy);
                        if (gl > 0.0001f) { gx /= gl; gy /= gl; }
                    }
                    ox = d * gx;
                    oy = d * gy;
                    /* Highlight.Default: intensity = |dot(grad, normal45)|,
                     * falloff 1 — bright where the rim faces the 45° light */
                    hl = gx * HL_ANGLE_COS + gy * HL_ANGLE_SIN;
                    hl = hl < 0.0f ? -hl : hl;
                    /* InnerShadow(8dp): dark ring just inside the rim */
                    float t = 1.0f - (-sd) / INNER_R;
                    if (t > 0.0f) {
                        t *= t;
                        ish = t * INNER_A;
                    }
                }
            }

            /* ---- backdrop sample (with optional dispersion) ---- */
            int sbw = (w + GLASS_DS - 1) / GLASS_DS;
            int sbh = (h + GLASS_DS - 1) / GLASS_DS;
            uint32_t bg;
            if (ca > 0.0f && !fast) {
                /* dispersion: CA * (|x|*|y|)/(hw*hh) — strongest at the
                 * corners. 3-band approximation of upstream's 7 spectral
                 * bands (R+ / G0 / B-) at the refracted coordinate. */
                float gl = __builtin_sqrtf(ox * ox + oy * oy);
                float nx = gl > 0.0001f ? ox / gl : 0.0f;
                float ny = gl > 0.0001f ? oy / gl : 0.0f;
                float di = ca * ((dx < 0.0f ? -dx : dx) *
                                 (dy < 0.0f ? -dy : dy)) / (hw * hh);
                float off = gl * di;
                uint32_t sr_ = glass_sample(sbw, sbh,
                                            (i + ox + off * nx) / GLASS_DS,
                                            (j + oy + off * ny) / GLASS_DS);
                uint32_t sg_ = glass_sample(sbw, sbh,
                                            (i + ox) / GLASS_DS,
                                            (j + oy) / GLASS_DS);
                uint32_t sb_ = glass_sample(sbw, sbh,
                                            (i + ox - off * nx) / GLASS_DS,
                                            (j + oy - off * ny) / GLASS_DS);
                bg = AETHER_RGB(sr_ >> 16 & 255, sg_ >> 8 & 255, sb_ & 255);
            } else {
                bg = glass_sample(sbw, sbh, (i + ox) / GLASS_DS,
                                  (j + oy) / GLASS_DS);
            }
            int rr = bg >> 16 & 255, gg = bg >> 8 & 255, bb = bg & 255;

            /* vibrancy(): saturation 1.5x + slight brightness lift */
            int lum = (rr * 77 + gg * 150 + bb * 29) >> 8;
            rr = glass_clamp(lum + (int)((rr - lum) * VIBRANCY_SAT) + VIBRANCY_BRIGHT);
            gg = glass_clamp(lum + (int)((gg - lum) * VIBRANCY_SAT) + VIBRANCY_BRIGHT);
            bb = glass_clamp(lum + (int)((bb - lum) * VIBRANCY_SAT) + VIBRANCY_BRIGHT);

            /* self tint (Apple range 5%~35%) */
            rr += (int)((tr - rr) * TINT_MIX);
            gg += (int)((tg - gg) * TINT_MIX);
            bb += (int)((tb - bb) * TINT_MIX);

            /* AetherOS top sheen (weak, non-upstream) */
            float th = 1.0f - (float)j / (h * 0.30f);
            if (th < 0.0f) th = 0.0f;
            th = th * th * SHEEN_A;
            rr += (int)(255 * th); gg += (int)(255 * th); bb += (int)(255 * th);

            /* Highlight.Default (additive white @ 50% at the lit rim) */
            if (hl > 0.0f) {
                int add = (int)(255 * hl * hl_alpha);
                rr += add; gg += add; bb += add;
            }
            /* InnerShadow: dark bevel ring just inside the rim */
            rr -= (int)(70 * ish); gg -= (int)(70 * ish); bb -= (int)(80 * ish);
            rr = glass_clamp(rr); gg = glass_clamp(gg); bb = glass_clamp(bb);

            uint32_t orig = glass_snap[(size_t)j * w + i];   /* blend over */
            int orr = orig >> 16 & 255, ogg = orig >> 8 & 255, ob = orig & 255;
            float a = opacity * cov;
            row[i] = AETHER_RGB((int)(rr * a + orr * (1 - a)),
                                (int)(gg * a + ogg * (1 - a)),
                                (int)(bb * a + ob * (1 - a)));
        }
    }
}

/* Window material: upstream GlassMaterials.NavigationBar
 * = vibrancy() + blur(4dp) + lens(16dp, 32dp). */
void aether_render_glass(aether_fb_t *fb, int x, int y, int w, int h,
                         uint32_t tint, float radius, float opacity, float press)
{
    glass_panel(fb, x, y, w, h, tint, radius, opacity,
                WIN_LENS_H, WIN_LENS_A, 0.0f, WIN_BLUR_PASSES, press, 0);
}

/* System-layer panel material: upstream GlassMaterials.BottomSheet
 * = vibrancy() + blur(4dp) + lens(24dp, 48dp, depthEffect = true). */
void aether_render_glass_sheet(aether_fb_t *fb, int x, int y, int w, int h,
                               uint32_t tint, float radius, float opacity,
                               float press)
{
    glass_panel(fb, x, y, w, h, tint, radius, opacity,
                SHEET_LENS_H, SHEET_LENS_A, SHEET_DEPTH, SHEET_BLUR_PASSES,
                press, 0);
}

/* 全屏表面材质（锁屏/主屏/全屏应用铺满整块屏）：只有 backdrop blur +
 * vibrancy + 染色——没有可见的玻璃边，就不该有边缘折射/高光/阴影。
 * 上游的 lens 属于"悬浮面板"；全屏硬套会在四周画出一圈折射边框。 */
void aether_render_glass_full(aether_fb_t *fb, uint32_t tint, float opacity)
{
    glass_panel(fb, 0, 0, (int)fb->w, (int)fb->h, tint, 0.0f, opacity,
                0.0f, 0.0f, 0.0f, SHEET_BLUR_PASSES, 0.0f, 1);
}

void aether_render_window(aether_fb_t *fb, const aether_win_t *win)
{
    /* P4-1 全屏表面（锁屏/启动器/手机式应用）：无 chrome——
     * 系统层用 BottomSheet 配方（depthEffect 球状凸起），普通全屏应用
     * 用 NavigationBar 配方；客户端像素整窗 blit（TKEY 处透出玻璃）。 */
    if (win->fs) {
        int full = (win->x == 0 && win->y == 0 &&
                    win->w == (int)fb->w && win->h == (int)fb->h);
        if (full)
            /* 铺满整屏：无边玻璃（blur + vibrancy + 染色），见上方说明 */
            aether_render_glass_full(fb, AETHER_GLASS_TINT, 0.42f);
        else if (win->sys)
            aether_render_glass_sheet(fb, win->x, win->y, win->w, win->h,
                                      AETHER_GLASS_TINT, 0.0f, 0.62f,
                                      win->press);
        else
            aether_render_glass(fb, win->x, win->y, win->w, win->h,
                                AETHER_GLASS_TINT, 0.0f, 0.55f, win->press);
        if (win->has_shm && win->shm) {
            uint32_t stride = fb->pitch / 4;
            for (uint32_t yy = 0; yy < win->shm_h; yy++) {
                uint32_t *dst = fb->px + (size_t)(win->y + yy) * stride
                                + win->x;
                uint32_t *src = win->shm + (size_t)yy * win->shm_w;
                for (uint32_t xx = 0; xx < win->shm_w; xx++)
                    if (src[xx] != AETHER_TKEY)   /* P3-15: client-transparent */
                        dst[xx] = src[xx];
            }
        }
        return;
    }

    uint32_t border = win->focused
        ? AETHER_RGB(0x4F, 0xC3, 0xF7)   /* bright cyan: focus ring */
        : AETHER_RGB(0x3A, 0x4A, 0x5A);  /* dim slate */
    uint32_t title_fg = AETHER_RGB(0xE6, 0xED, 0xF3);
    uint32_t body_fg = AETHER_RGB(0xAE, 0xBA, 0xC4);

    /* P3-15/P4-1: liquid-glass body. Frost whatever is behind this window
     * (desktop + lower windows) into a translucent tinted panel.
     * Material: upstream GlassMaterials.NavigationBar. */
    aether_render_glass(fb, win->x, win->y, win->w, win->h,
                        AETHER_GLASS_TINT, 16.0f, 0.5f, win->press);
    /* border: outline */
    for (int i = 0; i < AETHER_BORDER_W; i++) {
        aether_render_rect(fb, win->x + i, win->y + i,
                           win->w - 2 * i, 1, border);
        aether_render_rect(fb, win->x + i, win->y + win->h - 1 - i,
                           win->w - 2 * i, 1, border);
        aether_render_rect(fb, win->x + i, win->y + i,
                           1, win->h - 2 * i, border);
        aether_render_rect(fb, win->x + win->w - 1 - i, win->y + i,
                           1, win->h - 2 * i, border);
    }
    /* focus dot in the titlebar */
    if (win->focused)
        aether_render_rect(fb, win->x + 8, win->y + 9, 6, 6,
                           AETHER_RGB(0x4F, 0xC3, 0xF7));

    aether_render_text(fb, win->x + 20, win->y + 4, win->title, title_fg);

    int ty = win->y + AETHER_TITLEBAR_H + 8;
    for (int i = 0; i < win->n_lines; i++) {
        aether_render_text(fb, win->x + 10, ty, win->lines[i], body_fg);
        ty += AETHER_LINE_H;
    }

    /* client-provided pixel buffer blitted into the content area
     * (titlebar + border reserved). Shm size is bounded by the window
     * chrome, so no overdraw beyond the window is possible. */
    if (win->has_shm && win->shm) {
        int bx = win->x + AETHER_BORDER_W;
        int by = win->y + AETHER_TITLEBAR_H + AETHER_BORDER_W;
        uint32_t stride = fb->pitch / 4;
        for (uint32_t yy = 0; yy < win->shm_h; yy++) {
            uint32_t *dst = fb->px + (size_t)(by + yy) * stride + bx;
            uint32_t *src = win->shm + (size_t)yy * win->shm_w;
            for (uint32_t xx = 0; xx < win->shm_w; xx++)
                if (src[xx] != AETHER_TKEY)   /* P3-15: client-transparent */
                    dst[xx] = src[xx];
        }
    }
}

/* ---- P4-1: 放大字模（锁屏大时钟） ------------------------------------
 * 8x16 ASCII / 16x16 CJK 位图按 scale 倍最近邻放大。scale<=1 时与
 * aether_render_text_utf8 完全一致。返回推进宽度（px）。 */
static void blit_glyph8_s(aether_fb_t *fb, int x, int y,
                          const unsigned char *g, uint32_t rgb, int s)
{
    uint32_t stride = fb->pitch / 4;
    for (int gy = 0; gy < 16; gy++) {
        unsigned char bits = g[gy];
        for (int gx = 0; gx < 8; gx++) {
            if (!(bits & (0x80u >> gx)))
                continue;
            for (int sy = 0; sy < s; sy++) {
                int py = y + gy * s + sy;
                if (py < 0 || py >= (int)fb->h)
                    continue;
                uint32_t *row = fb->px + (size_t)py * stride;
                for (int sx = 0; sx < s; sx++) {
                    int px = x + gx * s + sx;
                    if (px < 0 || px >= (int)fb->w)
                        continue;
                    row[px] = rgb;
                }
            }
        }
    }
}

static void blit_glyph16_s(aether_fb_t *fb, int x, int y,
                           const uint8_t *g, uint32_t rgb, int s)
{
    uint32_t stride = fb->pitch / 4;
    for (int gy = 0; gy < 16; gy++) {
        uint32_t row16 = ((uint32_t)g[2 * gy] << 8) | g[2 * gy + 1];
        for (int gx = 0; gx < 16; gx++) {
            if (!(row16 & (1u << (15 - gx))))
                continue;
            for (int sy = 0; sy < s; sy++) {
                int py = y + gy * s + sy;
                if (py < 0 || py >= (int)fb->h)
                    continue;
                uint32_t *row = fb->px + (size_t)py * stride;
                for (int sx = 0; sx < s; sx++) {
                    int px = x + gx * s + sx;
                    if (px < 0 || px >= (int)fb->w)
                        continue;
                    row[px] = rgb;
                }
            }
        }
    }
}

int aether_render_text_scaled(aether_fb_t *fb, int x, int y, const char *s,
                              uint32_t rgb, int scale)
{
    if (scale < 1)
        scale = 1;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    int start_x = x;
    while (*p) {
        uint32_t cp;
        int adv;
        unsigned char c = *p;
        if (c < 0x80) {
            cp = c; p++; adv = 8 * scale;
        } else if ((c & 0xE0) == 0xC0 && p[1]) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2; adv = 16 * scale;
        } else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((uint32_t)(c & 0x0F) << 12) |
                 ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3; adv = 16 * scale;
        } else {
            p++;
            continue;
        }
        if (cp <= 0x7F) {
            if (cp >= 0x20)
                blit_glyph8_s(fb, x, y, aether_font_8x16[cp - 0x20], rgb,
                              scale);
        } else {
            int idx = cjk_find(cp);
            if (idx >= 0)
                blit_glyph16_s(fb, x, y, aether_cjk_glyphs[idx], rgb, scale);
            else {
                int w = 16 * scale;
                for (int t = 0; t < w; t++) {
                    aether_render_rect(fb, x + t, y, 1, scale, rgb);
                    aether_render_rect(fb, x + t, y + w - scale, 1, scale,
                                       rgb);
                }
                for (int t = 0; t < w; t += scale) {
                    aether_render_rect(fb, x, y + t, scale, 1, rgb);
                    aether_render_rect(fb, x + w - scale, y + t, scale, 1,
                                       rgb);
                }
            }
        }
        x += adv;
    }
    return x - start_x;
}

/* ---- P4-1: 触摸反馈圆点 --------------------------------------------
 * 双环软圆（r=18 淡 + r=10 浓），alpha 0..1。手指原生的可见确认：
 * 鼠标指针不可见时（AVNC 触摸模式/纯平板），每次触摸都有反馈。 */
void aether_render_touch_dot(aether_fb_t *fb, int x, int y, float alpha)
{
    if (alpha <= 0.0f)
        return;
    if (alpha > 1.0f)
        alpha = 1.0f;
    uint32_t stride = fb->pitch / 4;
    struct ring { int r; float a; };
    static const struct ring rings[2] = { { 18, 0.10f }, { 10, 0.28f } };
    for (int ri = 0; ri < 2; ri++) {
        int r = rings[ri].r;
        float a = rings[ri].a * alpha;
        for (int dy = -r; dy <= r; dy++) {
            int py = y + dy;
            if (py < 0 || py >= (int)fb->h)
                continue;
            uint32_t *row = fb->px + (size_t)py * stride;
            for (int dx = -r; dx <= r; dx++) {
                int px = x + dx;
                if (px < 0 || px >= (int)fb->w)
                    continue;
                float d2 = (float)(dx * dx + dy * dy) / (float)(r * r);
                if (d2 > 1.0f)
                    continue;
                float fall = 1.0f - d2;          /* 中心实、边缘虚 */
                float k = a * fall;
                uint32_t p = row[px];
                int pr = p >> 16 & 255, pg = p >> 8 & 255, pb = p & 255;
                pr += (int)((255 - pr) * k);
                pg += (int)((255 - pg) * k);
                pb += (int)((255 - pb) * k);
                row[px] = AETHER_RGB(pr, pg, pb);
            }
        }
    }
}

/* ---- 彩色壁纸 ------------------------------------------------------
 * 液态玻璃需要背后有彩色、有层次的背景，磨砂模糊才能真正"透"出玻璃感
 * （iOS Liquid Glass 的观感前提）。纯黑/暗蓝底会让透明窗口只剩发暗，看
 * 不出玻璃。这里画对角多段渐变 + 两处柔和径向辉光。
 *
 * 性能（TCG 关键）：逐像素计算较贵，故按分辨率缓存一次，之后每帧仅
 * memcpy。分辨率变化（TCG/virgl 切换）才重算。 */

#define WALL_MAXPX (1280u * 1600u)

static uint32_t wall_cache[WALL_MAXPX];
static uint32_t wall_cache_w, wall_cache_h;   /* 0 = 未生成 */

/* ---- 位图壁纸（P6 PPM）--------------------------------------------
 * 手机产品形态需要「真实照片」作底：纯渐变底上玻璃的模糊/折射/色差
 * 几乎看不出来（没有细节可扭曲）。合成器启动时加载一张 PPM 壁纸，
 * 按 cover 等比铺满 + 居中裁剪（不拉伸变形）；缺失则回退上面的渐变
 * （诚实降级：没壁纸也不黑屏、不假装加载成功）。 */
static uint8_t *wall_img;        /* RGB888 */
static int wall_img_w, wall_img_h;

int aether_render_load_wallpaper(const char *path)
{
    FILE *f;
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    size_t n;
    uint8_t *buf;

    if (!path || !*path) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0 ||
        fscanf(f, "%d %d %d", &w, &h, &maxv) != 3 ||
        w <= 0 || h <= 0 || maxv != 255 ||
        (uint64_t)w * (uint64_t)h > 8192ull * 8192ull) {
        fclose(f);
        return -1;
    }
    fgetc(f);                       /* maxval 后恰一个空白字节 */
    n = (size_t)w * (size_t)h * 3u;
    buf = malloc(n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, n, f) != n) { free(buf); fclose(f); return -1; }
    fclose(f);
    free(wall_img);
    wall_img = buf;
    wall_img_w = w;
    wall_img_h = h;
    wall_cache_w = wall_cache_h = 0;   /* 尺寸未变也要作废缓存 */
    return 0;
}

int aether_render_wallpaper_size(int *w, int *h)
{
    if (!wall_img) return -1;
    if (w) *w = wall_img_w;
    if (h) *h = wall_img_h;
    return 0;
}

/* cover 采样：等比放大到铺满，居中裁剪；逐目标行/列用 16.16 步进
 * 预先算好源坐标，避免每像素除法（TCG 下除法很贵，这里只算一次）。 */
static void wall_blit_cover(uint32_t *dst, uint32_t stride,
                            uint32_t w, uint32_t h)
{
    uint64_t scale = ((uint64_t)w << 16) / (uint32_t)wall_img_w;
    uint64_t sy = ((uint64_t)h << 16) / (uint32_t)wall_img_h;
    uint32_t sw, sh, sx0, sy0;

    if (sy > scale) scale = sy;     /* 取较大比例 => cover */
    sw = (uint32_t)(((uint64_t)w << 16) / scale);
    sh = (uint32_t)(((uint64_t)h << 16) / scale);
    if (sw > (uint32_t)wall_img_w) sw = (uint32_t)wall_img_w;
    if (sh > (uint32_t)wall_img_h) sh = (uint32_t)wall_img_h;
    sx0 = ((uint32_t)wall_img_w - sw) / 2;
    sy0 = ((uint32_t)wall_img_h - sh) / 2;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = dst + (size_t)y * stride;
        const uint8_t *src = wall_img +
            (size_t)(sy0 + (uint32_t)(((uint64_t)y * sh) / h)) *
                (size_t)wall_img_w * 3u;
        uint32_t sx = sx0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = src + (size_t)sx * 3u;
            row[x] = AETHER_RGB(p[0], p[1], p[2]);
            sx = sx0 + (uint32_t)(((uint64_t)(x + 1) * sw) / w);
        }
    }
}

static void wall_gen(uint32_t *dst, uint32_t stride, uint32_t w, uint32_t h)
{
    /* 5 个渐变停靠点：深靛 -> 紫罗兰 -> 品红 -> 玫橙 -> 青 */
    static const uint32_t stops[5] = {
        AETHER_RGB(0x24, 0x1B, 0x52),
        AETHER_RGB(0x4E, 0x2A, 0x8E),
        AETHER_RGB(0x9B, 0x3C, 0x82),
        AETHER_RGB(0xC7, 0x5E, 0x5E),
        AETHER_RGB(0x24, 0x6E, 0x7C),
    };
    /* 两处径向辉光：右上冷蓝、左下暖橙（平方衰减，免开方） */
    struct glow { int cx, cy, r2, r, g, b, strength; };
    int gr = (int)(w / 2);
    struct glow glows[2] = {
        { (int)(w * 3 / 4), (int)(h / 5), gr * gr,
          0x5A, 0x7D, 0xFF, 96 },
        { (int)(w / 6), (int)(h * 4 / 5), gr * gr,
          0xFF, 0x7A, 0x4D, 72 },
    };
    uint32_t denom = (w + h) ? (w + h) : 1;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = dst + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            /* 对角参数 t 归一化到 0..1024（跨 4 段） */
            uint32_t t256 = (uint32_t)(((uint64_t)(x + y) * 1024u) / denom);
            uint32_t seg = t256 / 256;
            if (seg > 3) seg = 3;
            uint32_t lt = t256 - seg * 256;
            uint32_t c0 = stops[seg], c1 = stops[seg + 1];
            int r = (int)(((c0 >> 16) & 0xFF) * (256 - lt) +
                          ((c1 >> 16) & 0xFF) * lt) / 256;
            int g = (int)(((c0 >> 8) & 0xFF) * (256 - lt) +
                          ((c1 >> 8) & 0xFF) * lt) / 256;
            int b = (int)((c0 & 0xFF) * (256 - lt) +
                          (c1 & 0xFF) * lt) / 256;
            for (int gi = 0; gi < 2; gi++) {
                int dx = (int)x - glows[gi].cx;
                int dy = (int)y - glows[gi].cy;
                int d2 = dx * dx + dy * dy;
                if (d2 < glows[gi].r2) {
                    int f = (int)((long long)(glows[gi].r2 - d2) * 256 /
                                  glows[gi].r2);
                    f = (f * f) / 256;              /* 平方衰减更柔和 */
                    f = (f * glows[gi].strength) / 256;
                    r += (glows[gi].r - r) * f / 256;
                    g += (glows[gi].g - g) * f / 256;
                    b += (glows[gi].b - b) * f / 256;
                }
            }
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void aether_render_wallpaper(aether_fb_t *fb)
{
    uint32_t stride = fb->pitch / 4;
    if ((uint64_t)fb->w * fb->h <= WALL_MAXPX) {
        if (wall_cache_w != fb->w || wall_cache_h != fb->h) {
            if (wall_img)
                wall_blit_cover(wall_cache, fb->w, fb->w, fb->h);
            else
                wall_gen(wall_cache, fb->w, fb->w, fb->h);
            wall_cache_w = fb->w;
            wall_cache_h = fb->h;
        }
        for (uint32_t y = 0; y < fb->h; y++)
            memcpy(fb->px + (size_t)y * stride,
                   wall_cache + (size_t)y * fb->w, (size_t)fb->w * 4);
    } else if (wall_img) {
        wall_blit_cover(fb->px, stride, fb->w, fb->h);
    } else {
        wall_gen(fb->px, stride, fb->w, fb->h);
    }
}

void aether_render_desktop(aether_fb_t *fb)
{
    aether_render_wallpaper(fb);
    /* P1-1b: Chinese status line on the desktop. Rendered every frame from
     * the curated 16x16 CJK bitmap subset (Noto Sans CJK SC, source-traced).
     * Placed in the bottom band (x=40, y=H-48) which is outside every window
     * rectangle, so it is never overdrawn. boot-smoke J16 derives its scan
     * box from the KMS mode (1280x800 -> y=752; 720x1600 -> y=1552). */
    int ty = (int)fb->h - 48;
    if (ty < 0) ty = 0;
    aether_render_text_utf8(fb, 40, ty, AETHER_DESKTOP_CN,
                           AETHER_RGB(0xE6, 0xED, 0xF3));
}
