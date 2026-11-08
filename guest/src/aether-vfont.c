/*
 * aether-vfont.c - 矢量字体栅格化（stb_truetype + 内嵌 TrueType 子集）
 *
 * 开源归属：
 *   - 字形 Noto Sans CJK SC, SIL Open Font License 1.1（修改版，见 assets/OFL.txt）
 *   - 栅格化 stb_truetype.h v1.08, public domain (Sean Barrett / RAD Game Tools)
 *   - 轮廓转换 fontTools (MIT)，离线完成，运行时不依赖
 *
 * 字体数据由构建脚本用 objcopy 从 assets/aether-font.ttf 生成同名符号的
 * 目标文件并链接进本程序，因此运行时不读取任何外部文件。
 */
#include "aether-vfont.h"

/* stb 的实现单元会保留若干本工程未调用的辅助函数；抑制由此产生的
 * -Wunused-function 噪声，其余警告照常保留。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#include <stdlib.h>
#include <string.h>

extern const unsigned char _binary_aether_font_ttf_start[];
extern const unsigned char _binary_aether_font_ttf_end[];

static stbtt_fontinfo g_font;
static int g_ready = -1;      /* -1 未初始化, 0 失败, 1 成功 */
static int g_asc, g_desc, g_gap;

/* 字形缓存：开放寻址，容量取 2 的幂。UI 文案与字号组合远小于此。 */
#define CACHE_N 4096u
static struct aether_glyph g_cache[CACHE_N];
static unsigned g_cached = 0;

static int vfont_init(void)
{
    if (g_ready >= 0) return g_ready;
    g_ready = 0;
    const unsigned char *data = _binary_aether_font_ttf_start;
    size_t len = (size_t)(_binary_aether_font_ttf_end - _binary_aether_font_ttf_start);
    if (len < 12) return 0;
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || !stbtt_InitFont(&g_font, data, off)) return 0;
    stbtt_GetFontVMetrics(&g_font, &g_asc, &g_desc, &g_gap);
    g_ready = 1;
    return 1;
}

static unsigned cache_hash(unsigned cp, int px_h)
{
    unsigned k = cp * 2654435761u ^ (unsigned)px_h * 40503u;
    return k & (CACHE_N - 1u);
}

static void cache_clear(void)
{
    for (unsigned i = 0; i < CACHE_N; i++) {
        if (g_cache[i].cp) {
            free(g_cache[i].alpha);
            memset(&g_cache[i], 0, sizeof(g_cache[i]));
        }
    }
    g_cached = 0;
}

/* 栅格化到空槽。返回 1 成功（含无轮廓的空格），0 表示该码点无字形。 */
static int cache_fill(struct aether_glyph *g, unsigned cp, int px_h)
{
    int gid = stbtt_FindGlyphIndex(&g_font, (int)cp);
    if (gid == 0) return 0;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px_h);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&g_font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;

    if (w > 0 && h > 0) {
        unsigned char *bm = (unsigned char *)malloc((size_t)w * (size_t)h);
        if (!bm) return 0;
        memset(bm, 0, (size_t)w * (size_t)h);
        stbtt_MakeCodepointBitmap(&g_font, bm, w, h, w, scale, scale, (int)cp);
        g->alpha = bm;
    } else {
        w = h = 0;
        g->alpha = NULL;
    }
    g->w = w;
    g->h = h;
    g->x0 = x0;
    g->y0 = y0;
    g->asc_px = (int)(g_asc * scale + 0.5f);
    g->cp = cp;
    g->px_h = px_h;
    g_cached++;
    return 1;
}

int aether_vfont_ready(void) { return vfont_init(); }

const struct aether_glyph *aether_vfont_get(unsigned cp, int px_h)
{
    if (!vfont_init() || cp == 0) return NULL;
    if (px_h < 1) px_h = 1;

    unsigned idx = cache_hash(cp, px_h);
    for (unsigned i = 0; i < CACHE_N; i++) {
        struct aether_glyph *g = &g_cache[(idx + i) & (CACHE_N - 1u)];
        if (g->cp == 0) {
            if (g_cached >= CACHE_N - 8u) {   /* 接近满：清空后重试 */
                cache_clear();
                return aether_vfont_get(cp, px_h);
            }
            return cache_fill(g, cp, px_h) ? g : NULL;
        }
        if (g->cp == cp && g->px_h == px_h) return g;
    }
    cache_clear();
    return aether_vfont_get(cp, px_h);
}
