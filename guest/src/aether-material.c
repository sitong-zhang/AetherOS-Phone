/*
 * aether-material.c - M1–M12 配方表、同帧模糊预算、全局按压反馈
 *
 * 纯逻辑层：不含画布写入，绘制仍由 aether-render.c / aether-glass.c 完成。
 * 见 aether-material.h 的规范依据与数值偏离说明。
 */
#include "aether-material.h"
#include "aether-anim.h"
#include <math.h>

/* ==================== M1–M12 配方表（分册 §7.2） ====================
 * blur_px / downsample / passes 按分册 §7.3 像素预算：
 *   轻(M3/M7) 8px  4× 1pass；中(M4/M5/M6/M12) 20px 4× 2pass；
 *   重(M9/M10) 32px 3× 3pass（仅在模态矩形内，禁止全屏）。
 * 圆角引用 TK_R_*；阴影档引用上游 glass_shadow.dart 语义；
 * tint_alpha 是深/浅两套的基准罩，调用方按主题做小幅增减（不新增第二套配方）。
 */
static const struct aether_mat_recipe RECIPES[AE_MAT_COUNT] = {
    /* id name                role                blur ds ps radius tint spec shadow          edge press */
    { AE_M1_WALL,     "壁纸底",     GLASS_ROLE_BACKGROUND,  0, 0, 0,  0,          0, 0, AE_SHADOW_NONE,   0, 0 },
    { AE_M2_CARD,     "内容卡",     GLASS_ROLE_CONTENT,     0, 0, 0, TK_R_LG,     0, 0, AE_SHADOW_DOUBLE,1, 0 },
    { AE_M3_STATUS,   "状态栏",     GLASS_ROLE_NAV,   TK_BLUR_LIGHT, 4, 1,  0,     0x10, 1, AE_SHADOW_NONE,   0, 0 },
    { AE_M4_DOCK,     "Dock",       GLASS_ROLE_NAV,   TK_BLUR_MED,   4, 2, TK_R_XL, 0x14, 1, AE_SHADOW_DOUBLE,1, 0 },
    { AE_M5_NOTIF,    "通知中心",   GLASS_ROLE_OVERLAY,TK_BLUR_MED,  4, 2, TK_R_XL, 0x10, 1, AE_SHADOW_DOUBLE,1, 0 },
    { AE_M6_CONTROL,  "控制中心",   GLASS_ROLE_OVERLAY,TK_BLUR_MED,  4, 2, TK_R_XL, 0x10, 1, AE_SHADOW_DOUBLE,1, 0 },
    { AE_M7_TILE_OFF, "磁贴关",     GLASS_ROLE_OVERLAY,TK_BLUR_LIGHT,4, 1, TK_R_MD, 0x0A, 1, AE_SHADOW_NONE,   1, 1 },
    { AE_M8_TILE_ON,  "磁贴开",     GLASS_ROLE_OVERLAY,TK_BLUR_LIGHT,4, 1, TK_R_MD, 0x2E, 1, AE_SHADOW_NONE,   1, 1 },
    { AE_M9_DIALOG,   "对话框",     GLASS_ROLE_MODAL, TK_BLUR_HEAVY, 3, 3, TK_R_XL, 0x0A, 1, AE_SHADOW_HEAVY,  1, 0 },
    { AE_M10_SHEET,   "二级面板",   GLASS_ROLE_MODAL, TK_BLUR_HEAVY, 3, 3, TK_R_2XL,0x10, 1, AE_SHADOW_HEAVY,  1, 0 },
    { AE_M11_TOAST,   "Toast",      GLASS_ROLE_OVERLAY, 0, 0, 0, TK_R_MD,     0xF0, 0, AE_SHADOW_DOUBLE,1, 0 },
    { AE_M12_ISLAND,  "灵动岛",     GLASS_ROLE_NAV,   TK_BLUR_MED,   4, 2,  0,     0xDB, 0, AE_SHADOW_DOUBLE,1, 1 },
};

const struct aether_mat_recipe *aether_material(int id)
{
    if (id < 1 || id > AE_MAT_COUNT) return NULL;
    return &RECIPES[id - 1];
}
int aether_material_count(void) { return AE_MAT_COUNT; }

int aether_material_allows_glass(int id)
{
    const struct aether_mat_recipe *r = aether_material(id);
    return r ? glass_role_allows_glass(r->role) : 0;
}
int aether_material_can_refract(int id, int parent_refracting)
{
    const struct aether_mat_recipe *r = aether_material(id);
    return r ? glass_role_can_refract(r->role, parent_refracting) : 0;
}

/* ==================== 同帧模糊预算（E02） ====================
 * 每帧只登记一次真算模糊：第一个请求返回 1（允许真算），同帧其余返回 0
 * （复用已有下采样盘子）。矩形信息只用于记录，不参与判定——软渲染环境下
 * 所有玻璃面共享同一块「磨砂壁纸盘子」，按矩形取交集复用是上游
 * ADAPTIVE_QUALITY.md 的规则，这里以「一帧一次」做等价且可断言的实现。 */
static int g_blur_budget = 0;
static int g_blur_rect[4] = { 0, 0, 0, 0 };
static int g_blur_radius = 0;

void aether_material_begin_frame(void)
{
    g_blur_budget = 0;
    g_blur_radius = 0;
}
int aether_material_request_blur(int x, int y, int w, int h, int radius)
{
    if (g_blur_budget >= 1) return 0;   /* 本帧已有盘子，复用 */
    g_blur_budget++;
    g_blur_rect[0] = x; g_blur_rect[1] = y;
    g_blur_rect[2] = w; g_blur_rect[3] = h;
    g_blur_radius = radius;
    return 1;
}
int aether_material_blur_budget(void) { return g_blur_budget; }

/* ==================== 全局按压反馈（§7.7 / C01） ====================
 * 进度 k∈[0,1]：down 当帧立即给一个可见的起始量（起手零延迟），
 * 随后按 TK_D_FAST(150ms) 用 ease_out 爬升到 1；up 后按
 * TK_D_REVERSE(60ms) 用 ease_in_out 回落到 0。
 * 提亮量 = 主题基准 × k；膨胀 = TK_PRESS_INFLATE × k（居中、不改布局）。 */
static int   g_touch_x = -1, g_touch_y = -1, g_touch_down = 0;
static float g_press = 0.0f;
static long  g_press_t0 = 0;
static float g_press_from = 0.0f, g_press_to = 0.0f;
static long  g_press_dur = 0;

#define PRESS_IMMEDIATE 0.22f   /* down 当帧的可见起始进度，避免等 150ms 才亮 */

static void press_start(float from, float to, long dur)
{
    g_press_from = from; g_press_to = to;
    g_press_dur = dur > 0 ? dur : 1;
    g_press_t0 = aether_anim_now_ms();
    g_press = from;
}

void aether_material_set_touch(int x, int y, int down)
{
    int was = g_touch_down;
    g_touch_x = x; g_touch_y = y; g_touch_down = down ? 1 : 0;
    if (aether_anim_reduce()) {        /* 减少动效：即时量，无过渡 */
        g_press = g_touch_down ? 1.0f : 0.0f;
        g_press_from = g_press_to = g_press; g_press_dur = 0;
        return;
    }
    if (down && !was) {
        /* down 当帧：直接跳到可见起始量，再淡入爬升（C01 硬约束） */
        press_start(PRESS_IMMEDIATE, 1.0f, TK_D_FAST);
    } else if (!down && was) {
        press_start(g_press, 0.0f, TK_D_REVERSE);
    }
}

void aether_material_tick(void)
{
    if (g_press_dur <= 0) return;
    long dt = aether_anim_now_ms() - g_press_t0;
    double t = (double)dt / (double)g_press_dur;
    if (t >= 1.0) { g_press = g_press_to; g_press_dur = 0; return; }
    double e = (g_press_to > g_press_from) ? aether_ease_out(t)
                                           : aether_ease_in_out(t);
    g_press = (float)(g_press_from + (g_press_to - g_press_from) * e);
}

int aether_material_pressed(int x, int y, int w, int h)
{
    return g_touch_down && g_touch_x >= x && g_touch_y >= y
           && g_touch_x < x + w && g_touch_y < y + h;
}
float aether_material_press_k(int theme)
{
    (void)theme;
    return g_press < 0.0f ? 0.0f : (g_press > 1.0f ? 1.0f : g_press);
}
int aether_material_press_inflate(void)
{
    return (int)(TK_PRESS_INFLATE * aether_material_press_k(0) + 0.5f);
}
float aether_material_press_lift(int theme)
{
    float base = theme == 1 ? TK_PRESS_LIFT_L : TK_PRESS_LIFT_D;
    return base * aether_material_press_k(theme);
}
