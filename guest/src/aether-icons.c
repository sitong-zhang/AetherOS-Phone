/*
 * aether-icons.c - AetherOS 开源图标绘制层
 *
 * 总纲第六章「图标铁律」：
 *   - 图标一律来自开源图标库（Lucide, ISC），禁止自绘/手绘/AI 生成/截图抠图；
 *   - 全系统只允许一套描边风格（Lucide 24x24 / stroke-width 2 / round cap）；
 *   - 应用图标统一为超椭圆，两层结构（玻璃底 + 上层单色线性符号）；
 *   - 同屏视觉重量一致，系统只施加统一遮罩与圆角，不逐个设计。
 *
 * 实现：alpha 掩膜来自 aether-icons-data.h（离线光栅化的开源矢量图标），
 * 运行时做双线性缩放，以任意前景色逐像素合成；放大后描边平滑、不失真。
 */
#include "aether-icons.h"
#include "aether-render.h"

/* 数据头仅在本编译单元展开一次（约 730KB 常量，进 .rodata） */
#include "aether-icons-data.h"

#include <stdio.h>
#include <string.h>

/* ---- 双线性采样 alpha 掩膜 ---- */
static unsigned char sample_alpha(const unsigned char *bits, double fx, double fy)
{
    if (fx < 0) fx = 0; if (fy < 0) fy = 0;
    if (fx > AE_ICON_W - 1) fx = AE_ICON_W - 1;
    if (fy > AE_ICON_H - 1) fy = AE_ICON_H - 1;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < AE_ICON_W ? x0 + 1 : x0;
    int y1 = y0 + 1 < AE_ICON_H ? y0 + 1 : y0;
    double tx = fx - x0, ty = fy - y0;
    double a = (double)bits[y0 * AE_ICON_W + x0] * (1 - tx) * (1 - ty)
             + (double)bits[y0 * AE_ICON_W + x1] * tx * (1 - ty)
             + (double)bits[y1 * AE_ICON_W + x0] * (1 - tx) * ty
             + (double)bits[y1 * AE_ICON_W + x1] * tx * ty;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return (unsigned char)a;
}

void aether_draw_icon(int id, int x, int y, int size, unsigned fg)
{
    if (id < 0 || id >= AE_ICON_COUNT || size <= 0) return;
    const unsigned char *bits = ae_icon_bits[id];
    for (int dy = 0; dy < size; dy++) {
        double fy = ((double)dy + 0.5) * AE_ICON_H / size - 0.5;
        for (int dx = 0; dx < size; dx++) {
            double fx = ((double)dx + 0.5) * AE_ICON_W / size - 0.5;
            unsigned char a = sample_alpha(bits, fx, fy);
            if (a) aether_blend_pixel(x + dx, y + dy, fg, a);
        }
    }
}

const char *aether_icon_name(int id)
{
    if (id < 0 || id >= AE_ICON_COUNT) return "";
    return ae_icon_names[id];
}

/* ---- 超椭圆（squircle）判定：四次超椭圆 |x/a|^4+|y/b|^4 <= 1 ---- */
static int in_squircle(int px, int py, int cx, int cy, double half)
{
    double dx = (double)(px - cx) / half;
    double dy = (double)(py - cy) / half;
    if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    if (dx > 1 || dy > 1) return 0;
    double x2 = dx * dx, y2 = dy * dy;
    return (x2 * x2 + y2 * y2) <= 1.0;
}

/* 应用图标磁贴：统一施加超椭圆玻璃底、边缘分离、顶部高光、单色线性符号。
 * 底板形状/圆角/高光由本函数统一决定，调用方只提供玻璃色与符号色，
 * 因此同屏所有图标视觉重量一致，不会出现某个特别花/重/亮。 */
void aether_draw_icon_tile(int id, int x, int y, int size,
                           unsigned glass, unsigned glyph,
                           const char *label, unsigned label_color)
{
    if (size <= 0) return;
    double cx = x + size / 2.0, cy = y + size / 2.0;
    double half = size / 2.0;

    /* 玻璃底：轻微半透明叠在壁纸上，边缘细分 */
    for (int py = y; py < y + size; py++) {
        for (int px = x; px < x + size; px++) {
            if (!in_squircle(px, py, (int)cx, (int)cy, half)) continue;
            /* 边缘 1px 分离线：稍暗一圈，制造浮起感 */
            int edge = !in_squircle(px, py, (int)cx, (int)cy, half - 1.5);
            unsigned base = edge ? pack_rgb(255, 255, 255) : glass;
            unsigned char a = edge ? 90 : 232;
            aether_blend_pixel(px, py, base, a);
        }
    }
    /* 顶部高光：上半部叠一层极淡白，形成玻璃反光，不改符号区 */
    for (int py = y; py < y + size / 2; py++) {
        for (int px = x; px < x + size; px++) {
            if (!in_squircle(px, py, (int)cx, (int)cy, half - 2)) continue;
            int fade = (size / 2 - (py - y)) * 90 / (size / 2);
            aether_blend_pixel(px, py, pack_rgb(255, 255, 255),
                               (unsigned char)(fade > 70 ? 70 : fade));
        }
    }
    /* 上层单色线性符号：占磁贴约 50%，居中，来自开源矢量图标 */
    int isz = size * 50 / 100;
    int ix = x + (size - isz) / 2;
    int iy = y + (size - isz) / 2;
    aether_draw_icon(id, ix, iy, isz, glyph);

    /* 名称标签：单行、过长省略，居中于磁贴下方 */
    if (label && label[0]) {
        char t[64];
        int avail = size + 16, wsum = 0, cut = 0;
        for (int k = 0; label[k] && cut < 60; k++) {
            int len = (unsigned char)label[k] < 0x80 ? 1 : 3;
            char tmp[5];
            memcpy(tmp, label + k, (size_t)len); tmp[len] = 0;
            int cw = aether_text_width_utf8(tmp);
            if (wsum + cw > avail) break;
            memcpy(t + cut, label + k, (size_t)len);
            cut += len; wsum += cw; k += len - 1;
        }
        t[cut] = 0;
        int nw = aether_text_width_utf8(t);
        aether_draw_text_utf8(x + (size - nw) / 2, y + size + 8, t, label_color);
    }
}
