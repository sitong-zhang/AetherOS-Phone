/*
 * aether-vfont.h - AetherOS 矢量字体栅格化接口
 *
 * 字形来源 Noto Sans CJK SC (SIL OFL 1.1) 的 TrueType 子集，由
 * tools/gen_vector_font.py 生成 assets/aether-font.ttf，构建脚本经 objcopy
 * 转为目标文件静态链入合成器；栅格化使用 stb_truetype.h v1.08 (public domain)。
 *
 * 设计约束（与旧 8x16 / 16x16 点阵保持布局兼容）：
 *   - 字符步进固定：ASCII 8px / CJK 16px，行盒高 16px，见 aether-render.h；
 *   - 字形按【基线】定位，完整落在行盒内，不再出现底部裁切；
 *   - 抗锯齿灰度位图由调用方按 alpha 混合到画布。
 */
#ifndef AETHER_VFONT_H
#define AETHER_VFONT_H

#include <stddef.h>

/*
 * 一个栅格化字形。cp / px_h 为内部缓存键（cp==0 表示空槽），
 * 其余字段供渲染层使用。
 */
struct aether_glyph {
    unsigned cp;
    int px_h;          /* 目标行盒像素高 */
    int w, h;          /* 灰度位图宽高（可为 0，如空格） */
    int x0, y0;        /* 位图左上角相对 pen / 基线的偏移（y0 通常为负） */
    int asc_px;        /* 该字号下 ascent 像素数：行盒顶 -> 基线 */
    unsigned char *alpha;  /* w*h 字节，0..255 覆盖率；缓存常驻，勿释放 */
};

/* 惰性初始化内嵌字体；成功返回 1。失败时调用方回退点阵。 */
int aether_vfont_ready(void);

/* 取字形（带缓存）。cp 为 Unicode 码点，px_h 为行盒像素高。
 * 返回 NULL 表示该码点无字形（调用方可回退 '?'）。 */
const struct aether_glyph *aether_vfont_get(unsigned cp, int px_h);

/* 取指定字号下的真实字体度量（分册 §5.3 基线公式的唯一来源）。
 * asc：行盒顶到基线的像素数；desc：基线到行盒底的像素数（正值）。
 * 二者相加约等于 size，供渲染层计算 baseline = y + (line_h-(asc+desc))/2 + asc。
 * 成功返回 1；字体不可用时返回 0，调用方按 8x16 点阵度量兜底。 */
int aether_vfont_metrics(int size, int *asc, int *desc);

#endif /* AETHER_VFONT_H */
