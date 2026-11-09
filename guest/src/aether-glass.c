/*
 * aether-glass.c - AetherOS 液态玻璃材质（开源实现照搬，C 软渲染等价实现）
 *
 * 数学公式、参数、降级规则来源见 aether-glass.h 头部版权/归属声明。
 * 原实现为 AGSL RuntimeShader (Kyant0) 与 GLSL Fragment Shader (sdegenaar)，
 * 依赖 GPU；本文件将其等价数学移植到 CPU 逐像素 XRGB8888 framebuffer。
 * 照搬关系（逐函数标注）：
 *
 *   glass_sd_rrect / glass_grad_rrect / glass_circle_map / glass_refraction_d
 *       <- Kyant0/AndroidLiquidGlass  internal/Shaders.kt  RoundedRectSDF,
 *          RoundedRectRefractionShaderString
 *   glass_disp_taps (7 段权重)
 *       <- Kyant0  RoundedRectRefractionWithDispersionShaderString
 *   glass_highlight
 *       <- Kyant0  DefaultHighlightShaderString + HighlightStyle.Default
 *                  (angle=45°, falloff=1, White alpha .5, Plus 混合)
 *   glass_saturate / LUMA_WEIGHTS
 *       <- sdegenaar  shaders/render.glsl  applySaturation (Rec.709)
 *   glass_apply_glass_color
 *       <- sdegenaar  shaders/render.glsl  applyGlassColor（adaptive 亮度保持）
 *   gaussian 通道 IIR 递推 (w *= g1; g1 *= g2)
 *       <- sdegenaar  shaders/progressive_blur.frag
 *   参数默认值
 *       <- sdegenaar  glass_defaults.dart / liquid_glass_settings.dart
 *       <- Kyant0    app/.../LiquidButton.kt（折射 H/A）
 *   三档降级 premium / standard / minimal
 *       <- sdegenaar  lib/types/glass_quality.dart + 总纲第七章
 *   层级规则「Glass 只用于浮层/导航，内容不透明」「不嵌套折射」
 *       <- sdegenaar  README / SKILL.md（见 aether-glass-spec.h）
 *
 * 逐条核对：third_party/liquid-glass/MAPPING.md
 */
#include "aether-glass.h"
#include "aether-render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ==================== 画布访问（由 render 层提供） ==================== */
static unsigned char *g_bits;
static int g_fw, g_fh, g_pitch;

static void glass_bind_framebuffer(void)
{
    aether_render_framebuffer(&g_bits, &g_fw, &g_fh, &g_pitch);
}

static inline unsigned px_get(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_fw || y >= g_fh) return 0;
    return ((unsigned *)(g_bits + (size_t)y * g_pitch))[x];
}
static inline void px_set(int x, int y, unsigned v)
{
    if (x < 0 || y < 0 || x >= g_fw || y >= g_fh) return;
    ((unsigned *)(g_bits + (size_t)y * g_pitch))[x] = v;
}
static inline unsigned pack3(float r, float g, float b)
{
    int R = (int)(r + 0.5f), G = (int)(g + 0.5f), B = (int)(b + 0.5f);
    if (R < 0) { R = 0; }
    if (R > 255) { R = 255; }
    if (G < 0) { G = 0; }
    if (G > 255) { G = 255; }
    if (B < 0) { B = 0; }
    if (B > 255) { B = 255; }
    return pack_rgb((unsigned char)R, (unsigned char)G, (unsigned char)B);
}

/* ==================== 质量档位 ==================== */
static int g_quality = GLASS_PREMIUM;
void aether_glass_set_quality(int q)
{
    g_quality = q < GLASS_MINIMAL ? GLASS_MINIMAL :
                q > GLASS_PREMIUM ? GLASS_PREMIUM : q;
}
int aether_glass_quality(void) { return g_quality; }

/* ==================== 纯数学（照搬 AGSL/GLSL） ==================== */

/* sdRoundedRect / gradSdRoundedRect <- Kyant0 Shaders.kt RoundedRectSDF
 * 统一圆角 r（总纲 6.2「圆角统一」），不使用按象限的四元圆角。 */
float glass_sd_rrect(float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(cx) - (hx - r);
    float qy = fabsf(cy) - (hy - r);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float outside = sqrtf(mx * mx + my * my) - r;
    float inside = (qx > qy ? qx : qy) < 0 ? (qx > qy ? qx : qy) : 0;
    return outside + inside;
}

void glass_grad_rrect(float cx, float cy, float hx, float hy, float r,
                      float *gx, float *gy)
{
    /* gradRadius = min(radius*1.5, min(halfSize.x,halfSize.y)) <- Shaders.kt */
    float gr = r * 1.5f;
    float m = hx < hy ? hx : hy;
    if (gr > m) gr = m;
    float qx = fabsf(cx) - (hx - gr);
    float qy = fabsf(cy) - (hy - gr);
    float sx = cx < 0 ? -1.0f : 1.0f, sy = cy < 0 ? -1.0f : 1.0f;
    if (qx >= 0 || qy >= 0) {
        float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
        float len = sqrtf(ax * ax + ay * ay);
        if (len < 1e-6f) { *gx = 0; *gy = 0; return; }
        *gx = sx * ax / len; *gy = sy * ay / len;
    } else {
        float t = (qy <= qx) ? 1.0f : 0.0f;   /* step(qy, qx) */
        *gx = sx * t; *gy = sy * (1.0f - t);
    }
}

/* circleMap(x) = 1 - sqrt(1 - x^2) <- Shaders.kt */
float glass_circle_map(float x)
{
    if (x < 0) { x = 0; }
    if (x > 1) { x = 1; }
    return 1.0f - sqrtf(1.0f - x * x);
}

/* d = circleMap(1 - (-sd)/H) * A <- RoundedRectRefractionShaderString */
float glass_refraction_d(float sd, float height, float amount)
{
    if (-sd >= height) return 0.0f;      /* 内部深处不折射 */
    if (sd > 0) sd = 0;
    return glass_circle_map(1.0f - (-sd) / height) * amount;
}

/* intensity = pow(abs(dot(grad, normal)), falloff) <- DefaultHighlightShaderString */
float glass_highlight(float gx, float gy, float angle_rad, float falloff)
{
    float nx = cosf(angle_rad), ny = sinf(angle_rad);
    float d = gx * nx + gy * ny;
    return powf(fabsf(d), falloff);
}

/* applySaturation: luma-preserving, Rec.709 <- render.glsl */
void glass_saturate(float *r, float *g, float *b, float saturation)
{
    float L = GLASS_LUMA_R * *r + GLASS_LUMA_G * *g + GLASS_LUMA_B * *b;
    *r = L + (*r - L) * saturation;
    *g = L + (*g - L) * saturation;
    *b = L + (*b - L) * saturation;
}

/* applyGlassColor (adaptive / Glass.regular) <- sdegenaar shaders/render.glsl
 *
 * iOS 26 模型：彩色玻璃保持背景亮度、只移动色相；无色玻璃趋向白。
 * 原 shader 在 0..1 域运算，此处等价换算到 0..255 域（亮度同域、chroma 归一）。
 *
 *   directMix      = mix(liquid, glass, a)
 *   backdropLuma   = dot(liquid, LUMA_WEIGHTS)
 *   glassLuma      = dot(glass,  LUMA_WEIGHTS)
 *   tinted         = clamp(glass + (backdropLuma - glassLuma), 0, 255)
 *   chroma         = max(glass) - min(glass)              （0..255）
 *   chromaWeight   = clamp(chroma / 255 * 8, 0, 1)        （锐利爬升）
 *   luminosityMix  = mix(liquid, tinted, a)
 *   result         = mix(directMix, luminosityMix, chromaWeight)
 *
 * 旧实现只是普通 alpha 混合却标注「亮度保持」，此函数补上真实语义。 */
static inline float clamp255(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 255.0f) return 255.0f;
    return v;
}

void glass_apply_glass_color(float *r, float *g, float *b,
                             float tr, float tg, float tb, float a)
{
    /* directMix */
    float dr = *r + (tr - *r) * a;
    float dg = *g + (tg - *g) * a;
    float db = *b + (tb - *b) * a;

    /* 亮度保持着色 */
    float backdrop_luma = GLASS_LUMA_R * *r + GLASS_LUMA_G * *g + GLASS_LUMA_B * *b;
    float glass_luma    = GLASS_LUMA_R * tr  + GLASS_LUMA_G * tg  + GLASS_LUMA_B * tb;
    float delta = backdrop_luma - glass_luma;
    float cr = clamp255(tr + delta);
    float cg = clamp255(tg + delta);
    float cb = clamp255(tb + delta);

    /* chroma 基于 glassColor【原始值】<- render.glsl：
     *   max(glassColor.rgb) - min(glassColor.rgb)
     * 不能用 tinted 后的 cr/cg/cb，否则无色玻璃会被误判为有彩度。 */
    float mx = tr > tg ? tr : tg; if (tb > mx) mx = tb;
    float mn = tr < tg ? tr : tg; if (tb < mn) mn = tb;
    float chroma_weight = (mx - mn) / 255.0f * 8.0f;
    if (chroma_weight > 1.0f) chroma_weight = 1.0f;

    float lr = *r + (cr - *r) * a;
    float lg = *g + (cg - *g) * a;
    float lb = *b + (cb - *b) * a;

    *r = dr + (lr - dr) * chroma_weight;
    *g = dg + (lg - dg) * chroma_weight;
    *b = db + (lb - db) * chroma_weight;
}

/* 7 段色散权重 <- RoundedRectRefractionWithDispersionShaderString
 * 原始 shader 把 red/orange/yellow/green/cyan/blue/purple 七次采样按
 * 1/3.5、1/7、1/3 的系数累加，alpha 各 1/7。此处等价为 RGB 权重表。 */
static const struct glass_disp_tap k_disp[GLASS_DISP_TAPS] = {
    {  1.0f,      1.0f/3.5f, 0.0f,       0.0f       },  /* red    +1    */
    {  2.0f/3.0f, 1.0f/3.5f, 1.0f/7.0f, 0.0f       },  /* orange +2/3 */
    {  1.0f/3.0f, 1.0f/3.5f, 1.0f/3.5f, 0.0f       },  /* yellow +1/3 */
    {  0.0f,      0.0f,      1.0f/3.5f, 0.0f       },  /* green   0   */
    { -1.0f/3.0f, 0.0f,      1.0f/3.5f, 1.0f/3.0f },  /* cyan   -1/3 */
    { -2.0f/3.0f, 0.0f,      0.0f,      1.0f/3.0f },  /* blue   -2/3 */
    { -1.0f,      1.0f/7.0f, 0.0f,      1.0f/3.0f },  /* purple -1   */
};
const struct glass_disp_tap *glass_disp_taps(void) { return k_disp; }

/* ==================== 可分离高斯（照搬 progressive_blur.frag） ====================
 * 与原 shader 相同：先降采样到小图控制 CPU 开销，在小图上做水平+垂直两遍
 * 可分离高斯；权重用 IIR 递推 w *= g1; g1 *= g2 代替逐 tap exp()。
 * 通道一律分离为 float 累加，避免旧实现整数 shift 串色（花屏根因）。 */
#define GLASS_DOWN 4
#define GLASS_KHALF 24

struct rgb_buf { int w, h; float *r, *g, *b; };

static struct rgb_buf *rgb_alloc(int w, int h)
{
    struct rgb_buf *p = (struct rgb_buf *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    size_t n = (size_t)w * h;
    p->w = w; p->h = h;
    p->r = (float *)malloc(n * 4); p->g = (float *)malloc(n * 4);
    p->b = (float *)malloc(n * 4);
    if (!p->r || !p->g || !p->b) { free(p->r); free(p->g); free(p->b); free(p); return NULL; }
    return p;
}
static void rgb_free(struct rgb_buf *p)
{
    if (!p) return;
    free(p->r); free(p->g); free(p->b); free(p);
}

/* 单轴高斯：horizontal=1 沿 x，=0 沿 y。IIR 权重递推照搬 progressive_blur.frag */
static void gaussian_axis(const struct rgb_buf *src, struct rgb_buf *dst,
                          float sigma, int horizontal)
{
    int w = src->w, h = src->h;
    int len = horizontal ? w : h;
    int lines = horizontal ? h : w;
    if (sigma < 0.5f) { memcpy(dst->r, src->r, (size_t)w*h*4);
                        memcpy(dst->g, src->g, (size_t)w*h*4);
                        memcpy(dst->b, src->b, (size_t)w*h*4); return; }
    float stride = 3.0f * sigma / (float)GLASS_KHALF;
    if (stride < 1.0f) stride = 1.0f;
    float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    float g1 = expf(-stride * stride * inv2s2);
    float g2 = g1 * g1;

    for (int line = 0; line < lines; line++) {
        for (int i = 0; i < len; i++) {
            size_t c = horizontal ? (size_t)line * w + i : (size_t)i * w + line;
            float ar = src->r[c], ag = src->g[c], ab = src->b[c];
            float sr = ar, sg = ag, sb = ab, wsum = 1.0f;
            float wk = 1.0f, a1 = g1, a2 = g2;
            for (int k = 1; k <= GLASS_KHALF; k++) {
                float d = (float)k * stride;
                int ip = i + (int)(d + 0.5f), im = i - (int)(d + 0.5f);
                wk *= a1; a1 *= a2;
                if (ip < len) {
                    size_t cp = horizontal ? (size_t)line * w + ip : (size_t)ip * w + line;
                    sr += src->r[cp] * wk; sg += src->g[cp] * wk; sb += src->b[cp] * wk;
                    wsum += wk;
                }
                if (im >= 0) {
                    size_t cm = horizontal ? (size_t)line * w + im : (size_t)im * w + line;
                    sr += src->r[cm] * wk; sg += src->g[cm] * wk; sb += src->b[cm] * wk;
                    wsum += wk;
                }
            }
            dst->r[c] = sr / wsum; dst->g[c] = sg / wsum; dst->b[c] = sb / wsum;
        }
    }
}

/* 从画布矩形降采样（box average）到小图 */
static struct rgb_buf *downsample(int x, int y, int w, int h)
{
    int sw = w / GLASS_DOWN, sh = h / GLASS_DOWN;
    if (sw < 2) { sw = 2; }
    if (sh < 2) { sh = 2; }
    struct rgb_buf *s = rgb_alloc(sw, sh);
    if (!s) return NULL;
    int step = GLASS_DOWN;
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) {
            int sx = x + i * w / sw, sy = y + j * h / sh;
            float R = 0, G = 0, B = 0; int n = 0;
            for (int dy = 0; dy < step; dy++) for (int dx = 0; dx < step; dx++) {
                unsigned v = px_get(sx + dx, sy + dy);
                R += (v >> 16) & 0xFF; G += (v >> 8) & 0xFF; B += v & 0xFF; n++;
            }
            size_t c = (size_t)j * sw + i;
            s->r[c] = R / n; s->g[c] = G / n; s->b[c] = B / n;
        }
    }
    return s;
}

/* 双线性采样小图：大图局部坐标 (u,v) -> 小图 (u/DOWN-0.5, v/DOWN-0.5) */
static void sample_small(const struct rgb_buf *s, float u, float v,
                         float *R, float *G, float *B)
{
    float fx = u / (float)GLASS_DOWN - 0.5f;
    float fy = v / (float)GLASS_DOWN - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - x0, ty = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) { x0 = 0; }
    if (x1 < 0) { x1 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (y1 < 0) { y1 = 0; }
    if (x0 >= s->w) { x0 = s->w - 1; }
    if (x1 >= s->w) { x1 = s->w - 1; }
    if (y0 >= s->h) { y0 = s->h - 1; }
    if (y1 >= s->h) { y1 = s->h - 1; }
    size_t a = (size_t)y0 * s->w + x0, b = (size_t)y0 * s->w + x1;
    size_t c = (size_t)y1 * s->w + x0, d = (size_t)y1 * s->w + x1;
    #define BIL(F) ((F)[a]*(1-tx)*(1-ty) + (F)[b]*tx*(1-ty) + \
                    (F)[c]*(1-tx)*ty     + (F)[d]*tx*ty)
    *R = BIL(s->r); *G = BIL(s->g); *B = BIL(s->b);
    #undef BIL
}

/* ==================== 画布级合成 ==================== */

/* 核心：把区域 (x,y,w,h) 的下层像素做成液态玻璃浮层。
 * 流程严格照搬开源渲染管线：
 *   1 模糊 pass（可分离高斯）           <- Flutter Architecture「Blur pass」
 *   2 折射/色散 pass（仅 premium）      <- Flutter「Shader pass」/ Kyant0 lens
 *   3 法线高光 + Rec.709 饱和度         <- render.glsl / Highlight
 *   4 色调罩（亮度保持，保证可读性）   <- applyGlassColor + 总纲 5.4
 */
static void glass_composite(int x, int y, int w, int h,
                            int rect_radius, int blur_radius,
                            unsigned tint, unsigned char tint_a,
                            unsigned highlight_rgb, unsigned solid_rgb,
                            int quality)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    glass_bind_framebuffer();
    if (!g_bits) return;
    if (x + w > g_fw) w = g_fw - x;
    if (y + h > g_fh) h = g_fh - y;
    if (w <= 0 || h <= 0) return;

    /* MINIMAL：无障碍高对比 / 性能兜底 -> 实色（总纲 5.20、第七章最后一级） */
    if (quality <= GLASS_MINIMAL) {
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++)
            px_set(x + i, y + j, solid_rgb);
        return;
    }

    struct rgb_buf *s = downsample(x, y, w, h);
    if (!s) return;
    struct rgb_buf *t = rgb_alloc(s->w, s->h);
    if (!t) { rgb_free(s); return; }
    /* sigma 按半径缩放；standard 档先降模糊（总纲第七章「先降模糊」） */
    float sigma = (float)blur_radius / (float)GLASS_DOWN;
    if (quality == GLASS_STANDARD) sigma *= 0.55f;
    if (sigma < 0.5f) sigma = 0.5f;
    gaussian_axis(s, t, sigma, 1);
    gaussian_axis(t, s, sigma, 0);   /* s = 最终模糊小图 */

    float hx = (float)w * 0.5f, hy = (float)h * 0.5f;
    float rad = (float)rect_radius;
    if (rad > hx) { rad = hx; }
    if (rad > hy) { rad = hy; }
    /* 高光法线夹角 <- Kyant0 HighlightStyle.Default.angle = 45f
     * （GLASS_LIGHT_ANGLE_DEG 135° 是 sdegenaar 的光照方向，属另一量，不混用） */
    float angle = (float)(GLASS_HIGHLIGHT_ANGLE_DEG * 3.14159265358979 / 180.0);
    float tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
    float ta = (float)tint_a / 255.0f;
    float hr = (highlight_rgb >> 16) & 0xFF;
    float hg = (highlight_rgb >> 8) & 0xFF;
    float hb = highlight_rgb & 0xFF;
    const struct glass_disp_tap *disp = glass_disp_taps();
    int refract = (quality >= GLASS_PREMIUM);

    for (int j = 0; j < h; j++) {
        float cy = (float)j - hy;
        for (int i = 0; i < w; i++) {
            float cx = (float)i - hx;
            float u = (float)i, v = (float)j;
            float R, G, B;

            float sd = glass_sd_rrect(cx, cy, hx, hy, rad);
            float d = refract ? glass_refraction_d(sd, GLASS_REFRACTION_H,
                                                   GLASS_REFRACTION_A) : 0.0f;
            if (refract && d > 0.01f) {
                /* 边缘折射 + 7 段色散 <- RoundedRectRefractionWithDispersion */
                float gx, gy;
                glass_grad_rrect(cx, cy, hx, hy, rad, &gx, &gy);
                float dispI = (cx * cy) / (hx * hy);   /* <- shader */
                float du = d * gx, dv = d * gy;
                R = G = B = 0;
                for (int k = 0; k < GLASS_DISP_TAPS; k++) {
                    float off = du * disp[k].offset * dispI;
                    float ofv = dv * disp[k].offset * dispI;
                    float sr, sg, sb;
                    sample_small(s, u + du + off, v + dv + ofv, &sr, &sg, &sb);
                    R += sr * disp[k].wr; G += sg * disp[k].wg; B += sb * disp[k].wb;
                }
            } else {
                sample_small(s, u, v, &R, &G, &B);
            }

            /* Rec.709 亮度保持饱和度 <- render.glsl */
            glass_saturate(&R, &G, &B, GLASS_SATURATION);

            /* 法线高光（Plus 混合，White alpha .5）<- Kyant0 DefaultHighlightShaderString
             * 光照强度 2.0 属 sdegenaar 的 directional lighting 模型，本 C 实现
             * 未引入该项，故不在此混乘，避免又一次跨库混搭。 */
            float gx, gy;
            glass_grad_rrect(cx, cy, hx, hy, rad, &gx, &gy);
            float hi = glass_highlight(gx, gy, angle, GLASS_HIGHLIGHT_FALLOFF)
                       * GLASS_HIGHLIGHT_ALPHA;
            R += hr * hi; G += hg * hi; B += hb * hi;

            /* 色调罩：亮度保持着色 <- render.glsl applyGlassColor（adaptive）
             * 旧实现为普通 alpha 混合却标注亮度保持，此处改为真实语义。 */
            glass_apply_glass_color(&R, &G, &B, tr, tg, tb, ta);

            px_set(x + i, y + j, pack3(R, G, B));
        }
    }
    rgb_free(t); rgb_free(s);
}

/* ---- 磨砂遮罩快路径（供桌面壁纸分层使用） ----
 * 只做「降采样 -> 可分离高斯 IIR -> 双线性放大」，结果写入调用方 dst
 * （XRGB8888，dst_pitch 字节/行），不触碰画布。刻意跳过 glass_composite
 * 的逐像素 SDF / 折射 / 7 段色散 / 法线高光 pow() / 亮度保持着色：那套
 * 循环在 720x1280 上是 92 万次重浮点运算，ARM64 TCG 会长时间占住串行 IPC
 * 主循环，watchdog 收不到 ping 的 pong 会误杀合成器。
 * 三个步骤全部复用上游 progressive_blur.frag 对应的 downsample /
 * gaussian_axis / sample_small，非自研。 */
void aether_glass_blur_veil(unsigned char *dst, int dst_pitch,
                            int x, int y, int w, int h, int radius)
{
    if (!dst || w <= 0 || h <= 0) return;
    glass_bind_framebuffer();
    if (!g_bits) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_fw) w = g_fw - x;
    if (y + h > g_fh) h = g_fh - y;
    if (w <= 0 || h <= 0 || dst_pitch < (x + w) * 4) return;

    struct rgb_buf *s = downsample(x, y, w, h);
    if (!s) return;
    struct rgb_buf *t = rgb_alloc(s->w, s->h);
    if (!t) { rgb_free(s); return; }
    float sigma = (float)radius / (float)GLASS_DOWN;
    if (sigma < 0.5f) sigma = 0.5f;
    gaussian_axis(s, t, sigma, 1);
    gaussian_axis(t, s, sigma, 0);      /* s = 最终模糊小图 */
    rgb_free(t);

    for (int j = 0; j < h; j++) {
        unsigned *row = (unsigned *)(dst + (size_t)(y + j) * dst_pitch);
        for (int i = 0; i < w; i++) {
            float R, G, B;
            sample_small(s, (float)i, (float)j, &R, &G, &B);
            row[x + i] = pack3(R, G, B);
        }
    }
    rgb_free(s);
}

/* ---- 公开 API ---- */
void aether_glass_blur_rect(int x, int y, int w, int h, int radius)
{
    /* 仅模糊、不叠色：供需要自定义合成的调用方 */
    glass_composite(x, y, w, h, 24, radius, pack_rgb(0, 0, 0), 0,
                    pack_rgb(0, 0, 0), pack_rgb(0, 0, 0), GLASS_STANDARD);
}

void aether_glass_panel(int x, int y, int w, int h, int rect_radius,
                        int blur_radius, unsigned tint, unsigned char tint_a,
                        unsigned highlight_rgb, unsigned solid_rgb)
{
    glass_composite(x, y, w, h, rect_radius, blur_radius, tint, tint_a,
                    highlight_rgb, solid_rgb, g_quality);
}

void aether_glass_blur_tint(int x, int y, int w, int h, int radius,
                            unsigned tint, unsigned char a)
{
    glass_composite(x, y, w, h, 24, radius, tint, a,
                    pack_rgb(255, 255, 255), pack_rgb(18, 24, 38),
                    g_quality < GLASS_PREMIUM ? g_quality : GLASS_STANDARD);
}

/* ==================== 控件级装饰（上游规范照搬） ====================
 * 应用内页面属「内容层」，按上游 Glass is a Platter / 四层结构规则保持不透明、
 * 不做折射玻璃；但控件语言（圆角、双层阴影、镜面高光、按压提亮）与浮层完全统一。
 * 阴影用「外扩的半透明圆角矩形 + 控件本体覆盖内部」实现上游「反向裁剪，
 * 阴影只落在边界之外」的观感，纯整数 alpha 叠加，TCG 上开销恒定。 */

unsigned aether_glass_lift(unsigned rgb, float k)
{
    int r = (int)((rgb >> 16) & 0xFF);
    int g = (int)((rgb >> 8) & 0xFF);
    int b = (int)(rgb & 0xFF);
    /* ambientLight：向白靠拢 k 比例（浅色 +0.30 / 深色 +0.14 luma） */
    r += (int)((255 - r) * k);
    g += (int)((255 - g) * k);
    b += (int)((255 - b) * k);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

void aether_glass_drop_shadow(int x, int y, int w, int h, int r)
{
    if (w <= 0 || h <= 0) return;
    /* [S] glass_shadow.dart：elevation 约 6% 黑 / blur 8 / dy 2；
     * contact 约 2% 黑 / blur 2 / dy 1。alpha 从 ARGB 最高字节提取。 */
    int eb = GLASS_SHADOW_ELEV_BLUR, cb = GLASS_SHADOW_CONTACT_BLUR;
    unsigned ea = (unsigned char)(GLASS_SHADOW_ELEV_ARGB >> 24);
    unsigned ca = (unsigned char)(GLASS_SHADOW_CONTACT_ARGB >> 24);
    /* 先画大范围 elevation，再画贴边 contact；调用方随后画不透明本体覆盖内部 */
    aether_draw_round_rect(x - eb / 2, y + GLASS_SHADOW_ELEV_DY,
                           w + eb, h + eb, r + eb / 2, 0x000000u, ea);
    aether_draw_round_rect(x - cb / 2, y + GLASS_SHADOW_CONTACT_DY,
                           w + cb, h + cb, r + cb / 2, 0x000000u, ca);
}

void aether_glass_specular_edge(int x, int y, int w, int h, int r, int theme)
{
    if (w <= 0 || h <= 0) return;
    float a = theme == 1 ? GLASS_SPECULAR_LIGHT : GLASS_SPECULAR_DARK;
    unsigned char hi = (unsigned char)(a * 255.0f);
    if (hi < 0x10) hi = 0x10;
    /* 顶边 1px 镜面高光，避让圆角起始段；底边 1px 暗线强化分层 */
    aether_fill_rect_alpha(x + r, y, w - 2 * r, 1, 0xFFFFFFu, hi);
    aether_fill_rect_alpha(x + r, y + h - 1, w - 2 * r, 1, 0x000000u,
                           (unsigned char)(hi / 2));
}

