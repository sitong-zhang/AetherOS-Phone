/*
 * aether-render.h - AetherOS 手机软件合成渲染层
 *
 * CPU 逐像素绘制到 DRM/fbdev 双缓冲（XRGB8888），不依赖任何图形库。
 * 提供：基础图元、ASCII/CJK 混合排版、控件图元、系统图标、液态玻璃材质。
 *
 * 输入形态：仅触摸（EV_ABS 绝对坐标 + BTN_TOUCH + 触控手势）。
 * PC 方案已废弃，本层不提供、不绘制任何鼠标指针；touch_x/touch_y 仅记录
 * 最后一个触点坐标，供 EV_ABS 分轴上报时累积与命中测试使用，不产生视觉指针。
 */
#ifndef AETHER_RENDER_H
#define AETHER_RENDER_H

#include <stddef.h>

struct render_fb {
    unsigned char *bits;
    int w, h;
    int pitch;
    int touch_x, touch_y;      /* 最后触点坐标（非指针，不绘制） */
};

void aether_render_init(unsigned char *bits, int w, int h, int pitch);
void aether_render_set_target(unsigned char *bits);
void aether_render_set_touch(int x, int y);

/* 只读查询当前画布（供液态玻璃等需要逐像素采样下层的材质模块使用） */
void aether_render_framebuffer(unsigned char **bits, int *w, int *h, int *pitch);

/* 全局绘制 alpha（255=不透明）：让整组图元做淡入淡出 / 依次入场，
 * 无需为每个控件单独传 alpha。绘制完务必复位 255。 */
void aether_render_set_alpha(unsigned char a);
unsigned char aether_render_alpha(void);
int  aether_render_touch_x(void);
int  aether_render_touch_y(void);

/* px = 0xFFRRGGBB；小端写入内存序 [B,G,R,0xFF]，与 DRM/fbdev 一致 */
static inline unsigned pack_rgb(unsigned char r, unsigned char g, unsigned char b)
{
    return (0xFFu << 24) | ((unsigned)r << 16) | ((unsigned)g << 8) | b;
}

/* ---- 基础图元 ---- */
/* 裁剪矩形：窗口内容渲染前设置，防止文字/控件溢出污染其他窗口 */
void aether_render_set_clip(int x, int y, int w, int h);
void aether_render_clear_clip(void);

void aether_clear(unsigned rgb);
void aether_fill_rect(int x, int y, int w, int h, unsigned rgb);
void aether_fill_rect_alpha(int x, int y, int w, int h, unsigned rgb, unsigned char a);
/* 按 alpha 把 rgb 混合到单个像素（供图标/玻璃材质逐像素合成，遵守裁剪与边界） */
void aether_blend_pixel(int x, int y, unsigned rgb, unsigned char a);

/* ---- 全局位移（统一转场 / 跟手动效） ----
 * 设置后所有图元坐标叠加 (dx,dy)，做「整层滑入 / 视差」无需改每处绘制；
 * 层内绘制完调用 aether_render_translate(0,0) 复位。 */
void aether_render_translate(int dx, int dy);

/* ---- 真算实时模糊（毛玻璃底层） ----
 * 对【当前画布已绘制内容】的矩形区域做可分离 box blur（降采样 + 多次迭代
 * 逼近高斯），结果写回画布。非静态贴图、非预烘焙，每帧按当下像素实时运算。
 * radius 为目标模糊半径（像素）；passes 通过材质档位自动控制。 */
void aether_blur_rect(int x, int y, int w, int h, int radius);
/* 模糊并叠加一层色调/暗罩，得到「背景→浮层」的层级分离 */
void aether_blur_tint(int x, int y, int w, int h, int radius, unsigned tint, unsigned char a);

/* 把画布当前区域快照到独立缓冲 / 从缓冲按 alpha 混合回画布（转场视差用） */
unsigned *aether_capture_rect(int x, int y, int w, int h);
void aether_blit_rect(unsigned *buf, int x, int y, int w, int h, unsigned char a);
void aether_free_capture(unsigned *buf);

/* ---- 文字 ---- */
void aether_draw_text(int x, int y, const char *s, unsigned rgb);          /* 纯 ASCII */
void aether_draw_text_utf8(int x, int y, const char *s, unsigned rgb);     /* UTF-8 混合 */
void aether_draw_text_scale(const char *s, int x, int y, int h, unsigned rgb); /* ASCII 按像素高 h 缩放 */
int  aether_text_width_scale(const char *s, int h);                        /* 与 draw_text_scale 同公式的缩放后宽度 */
int  aether_text_width(const char *s);                                     /* ASCII */
int  aether_text_width_utf8(const char *s);                                /* UTF-8 混合 */

/* ---- 字号感知排版（分册 §5.2 / §5.3） ----
 * 旧接口固定 8x16 行盒，业务层不得再写死「(h - 16) / 2」这类居中公式：
 * 矢量字体的真实 ascent/descent 与 16px 并不相等，descender 会被裁到行盒外。
 * 新接口按 aether_vfont_metrics() 的真实度量计算
 *   baseline = box_y + (line_h - (ascent + descent)) / 2 + ascent，
 * 且 line_h 自动抬升到 >= ascent + descent + 4（上下各 2px 光栅余量）。
 * aether_text_line_h()   该字号的行盒高（已含光栅余量）
 * aether_text_ascent()/descent()  真实字体度量
 * aether_text_box_y()     把行盒垂直居中于容器后，返回行盒顶部 y
 * aether_draw_text_utf8_sized()  按指定字号绘制，y 为行盒顶部
 * aether_draw_text_utf8_box()    盒内水平垂直居中 + 尾部省略号（不裁切、不半字）
 */
int  aether_text_line_h(int size);
int  aether_text_ascent(int size);
int  aether_text_descent(int size);
int  aether_text_box_y(int box_y, int box_h, int size);
int  aether_text_width_utf8_sized(const char *s, int size);
void aether_draw_text_utf8_sized(int x, int y, const char *s, int size, unsigned rgb);
/* 按 UTF-8 字符粒度截断并追加 U+2026 省略号（禁止半字、禁止 clip 代替） */
void aether_text_fit_sized(char *dst, size_t cap, const char *src, int size, int maxpx);
void aether_draw_text_utf8_box(int x, int y, int w, int h,
                               const char *s, int size, unsigned rgb);

/* ---- 控件图元 ---- */
void aether_draw_border(int x, int y, int w, int h, unsigned rgb, int t);
void aether_draw_round_rect(int x, int y, int w, int h, int r,
                            unsigned rgb, unsigned char alpha);
void aether_draw_vgradient(int x, int y, int w, int h, unsigned top, unsigned bot);

/* ---- 图标 ----（无光标/指针绘制，PC 方案已废弃） */
void aether_draw_logo(int cx, int cy, int size);

#endif /* AETHER_RENDER_H */
