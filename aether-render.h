/*
 * aether-render.h - software rasterizer for the AetherOS compositor.
 *
 * Paints XRGB8888 framebuffers provided by aether-drm (dumb buffer mmaps).
 * No GPU, no third-party library: rectangles, gradients, an 8x16 bitmap
 * font, and window chrome. Output is verifiable by dumping a PPM and
 * looking at it.
 */
#ifndef AETHER_RENDER_H
#define AETHER_RENDER_H

#include <stdint.h>

#define AETHER_RGB(r, g, b) \
    ((uint32_t)(((uint32_t)(r) & 0xFF) << 16) |      \
     (((uint32_t)(g) & 0xFF) << 8) |                 \
     (((uint32_t)(b) & 0xFF)))

/* Framebuffer view over a mapped dumb buffer. */
typedef struct {
    uint32_t *px;
    uint32_t w, h;
    uint32_t pitch; /* bytes per scanline */
} aether_fb_t;

/* One compositor window: chrome plus content.
 * Content is either server-rendered text (`lines`) for seeded/system
 * windows, OR a client-provided XRGB8888 pixel buffer shared over a
 * Unix socket via SCM_RIGHTS (`shm`). The two are mutually exclusive
 * per window. */
typedef struct {
    int id;
    const char *title;
    int x, y, w, h;
    int focused;
    /* P4-1: fullscreen surface — no chrome (border/titlebar); the client
     * buffer covers the whole window rectangle. Used by the SystemUI
     * lock screen / launcher and by phone-style app surfaces. */
    int fs;
    /* P4-1: system layer — renders BELOW normal windows (home/lock at the
     * bottom of the stack, like a phone's home screen) and receives taps
     * only when no normal window covers the point. */
    int sys;
    /* 0..1 interactive highlight (compositor sets per frame from the last
     * tap; decays over ~400ms). Drives the upstream InteractiveHighlight /
     * chromaticAberration-on-press behaviour. */
    float press;
    const char *const *lines;
    int n_lines;
    /* client-shared pixel buffer (XRGB8888), received via SCM_RIGHTS */
    uint32_t *shm;     /* NULL when server-rendered from `lines` */
    uint32_t shm_w, shm_h;
    int shm_fd;        /* backing memfd, -1 when none */
    int has_shm;
    /* P5: 普通窗口的 tap 转发目标（UDS 路径）。create 时以 sock=<path>
     * 可选参数注册；空表示不接收 tap（旧客户端行为不变）。 */
    char sockpath[108];
} aether_win_t;

#define AETHER_TITLEBAR_H 24
#define AETHER_BORDER_W 2
#define AETHER_LINE_H 18

void aether_render_init_fb(aether_fb_t *fb, void *map,
                           uint32_t w, uint32_t h, uint32_t pitch);

void aether_render_fill(aether_fb_t *fb, uint32_t rgb);
void aether_render_rect(aether_fb_t *fb, int x, int y, int w, int h,
                        uint32_t rgb);
void aether_render_gradient_v(aether_fb_t *fb, uint32_t top, uint32_t bottom);
void aether_render_text(aether_fb_t *fb, int x, int y, const char *s,
                        uint32_t rgb);
/* UTF-8 mixed text (P1-1b): ASCII -> 8x16, CJK -> 16x16 (binary search over
 * the curated subset). Invalid UTF-8 is skipped; CJK codepoints outside the
 * subset render as a tofu outline box. Returns advance width in pixels. */
void aether_render_text_utf8(aether_fb_t *fb, int x, int y, const char *s,
                             uint32_t rgb);
int aether_text_width_utf8(const char *s);
void aether_render_window(aether_fb_t *fb, const aether_win_t *win);

/* P3-15/P4-1 liquid glass. AETHER_TKEY is the client-transparent pixel key:
 * client apps that want the compositor's frosted-glass backdrop to show
 * through leave background pixels set to this value; aether_render_window()
 * skips them when blitting client content. Chosen so no UI element in the
 * tree uses it. */
#define AETHER_TKEY 0x00FF00FEU
#define AETHER_GLASS_TINT AETHER_RGB(0xB6, 0xD4, 0xF0)

/* P4-1: 液态玻璃底层（Kyant0/AndroidLiquidGlass = Apple Backdrop 2.0.0 的
 * 上游移植）。所有参数逐字取自上游"已洗好的配方"（docs/04 配方表），
 * 未做任何口味化改动；dp -> px 的密度换算见 AETHER_GLASS_DENSITY。
 *
 * 配方对应（GlassMaterials.kt verbatim）：
 *   窗口（普通应用窗口）  = NavigationBar: vibrancy() + blur(4dp) + lens(16dp,32dp)
 *   系统层面板（锁屏/启动器） = BottomSheet:  vibrancy() + blur(4dp) + lens(24dp,48dp,depthEffect)
 *
 * press: 0..1 —— 上游 InteractiveHighlight / chromaticAberration 只在交互
 * （按压/拖动）时开启；静止时色差为 0（静态常开会显得假）。合成器把最后一次
 * 触摸的衰减量传进来，实现"手指按下去玻璃才起液相反应"。 */
void aether_render_glass(aether_fb_t *fb, int x, int y, int w, int h,
                         uint32_t tint, float radius, float opacity, float press);
void aether_render_glass_sheet(aether_fb_t *fb, int x, int y, int w, int h,
                               uint32_t tint, float radius, float opacity,
                               float press);

/* P4-1: 放大字模（8x16 ASCII / 16x16 CJK 最近邻放大 scale 倍）——锁屏大时钟。
 * scale<=1 时退化为 aether_render_text_utf8。返回推进宽度（px）。 */
int aether_render_text_scaled(aether_fb_t *fb, int x, int y, const char *s,
                              uint32_t rgb, int scale);

/* P4-1: 触摸反馈圆点（手指原生的可见确认）：双环软圆，alpha 0..1 由合成器
 * 按 600ms 衰减传入。画在全部窗口之上。 */
void aether_render_touch_dot(aether_fb_t *fb, int x, int y, float alpha);

/* Desktop background used by the compositor each frame. */
void aether_render_desktop(aether_fb_t *fb);

/* 全屏表面材质（铺满整块屏的锁屏/主屏/全屏应用）：只有 backdrop blur +
 * vibrancy + 染色——屏幕上无可见玻璃边，故不做边缘折射/高光/阴影。 */
void aether_render_glass_full(aether_fb_t *fb, uint32_t tint, float opacity);

/* 位图壁纸（P6 PPM）：手机形态的真实照片底图——有细节可扭曲，玻璃的
 * 模糊/折射/色差才看得见。返回 0 成功；-1 时桌面回退内置渐变（诚实降级）。 */
int aether_render_load_wallpaper(const char *path);

/* 已加载壁纸的尺寸（未加载返回 -1），供日志如实报告。 */
int aether_render_wallpaper_size(int *w, int *h);

#endif /* AETHER_RENDER_H */
