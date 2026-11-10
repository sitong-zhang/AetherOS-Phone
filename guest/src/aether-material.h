/*
 * aether-material.h - AetherOS 材质配方层（M1–M12）与全局按压反馈
 *
 * 依据《AetherOS Phone UI/UX 规划书 V3.0》第 7 章「材质系统」与第 16 章
 * E02 断言落地；总纲 §6.1/§6.2/§7 只定效果（四层分离、实时真算模糊、
 * 全系统一套控件语言、性能不足自动降级），不规定具体数值。
 *
 * 本层只提供【配方数据、同帧模糊预算、按压进度】三类纯逻辑，绘制仍由
 * aether-phone.c / aether-ui.c 的既有原语完成，不改动已验证的
 * draw_overlay_backdrop 玻璃盘子缓存快路径（ARM64 TCG watchdog 边界）。
 *
 * 数值偏离说明（分册 §0 与总纲「只定效果不定参数」）：
 *   - 分册以 360×640dp / 1dp=2px 为基准；本系统 fbdev 固定 720×1280
 *     物理像素、纯 C CPU 逐像素渲染，M3–M10 的 blur 半径按
 *     §7.3「禁止全屏 3-pass 模糊」的像素预算实调（轻 8 / 中 20 / 重 32），
 *     降采样倍率与 passes 沿用上游 sdegenaar/liquid_glass_widgets 的
 *     progressive_blur.frag 语义，不整体 ×2。
 *   - M1 壁纸遮罩为条件式（按壁纸亮度插值），不是固定 0.35/0.45，
 *     实现见 aether-phone.c:draw_wallpaper() 与 TK_WALL_SCRIM_*。
 *   - 所有时长、圆角、阴影、提亮量均引用 aether-tokens.h /
 *     aether-glass-spec.h，本层不复制第二份数值。
 */
#ifndef AETHER_MATERIAL_H
#define AETHER_MATERIAL_H

#include "aether-glass-spec.h"   /* GLASS_ROLE_* 与上游光学常量 */
#include "aether-tokens.h"       /* TK_* 唯一真相源 */

/* ---- 12 种表面（分册 §7.2 配方表） ---- */
enum aether_mat {
    AE_M1_WALL      = 1,   /* 壁纸底：不模糊，条件式暗罩 */
    AE_M2_CARD      = 2,   /* 内容卡：不透明，禁止玻璃 */
    AE_M3_STATUS    = 3,   /* 状态栏：轻模糊 */
    AE_M4_DOCK      = 4,   /* Dock：中模糊 */
    AE_M5_NOTIF     = 5,   /* 通知中心面板：中模糊 */
    AE_M6_CONTROL   = 6,   /* 控制中心面板：中模糊 */
    AE_M7_TILE_OFF  = 7,   /* 磁贴关闭态：轻模糊低亮 */
    AE_M8_TILE_ON   = 8,   /* 磁贴开启态：accent 高亮 */
    AE_M9_DIALOG    = 9,   /* 对话框：重模糊 + 加重阴影 */
    AE_M10_SHEET    = 10,  /* 半高 Sheet / 二级面板：重模糊 */
    AE_M11_TOAST    = 11,  /* Toast / Snackbar：不模糊 */
    AE_M12_ISLAND   = 12   /* 灵动岛：真黑/真白胶囊 */
};
#define AE_MAT_COUNT 12

/* 阴影档：0 无 / 1 双层（6%+2%）/ 2 加重（12%+24%） */
#define AE_SHADOW_NONE   0
#define AE_SHADOW_DOUBLE 1
#define AE_SHADOW_HEAVY  2

struct aether_mat_recipe {
    int         id;
    const char *name;
    int         role;          /* GLASS_ROLE_*：准入判定用 */
    int         blur_px;       /* 目标模糊半径 px；0 = 不模糊 */
    int         downsample;    /* 降采样倍率（4× / 3×），0 = 不模糊 */
    int         passes;        /* 迭代次数 */
    int         radius_px;     /* 圆角 */
    int         tint_alpha;    /* 色调罩 alpha 0..255（深/浅由调用方按主题微调） */
    int         specular;      /* 顶边 1px 镜面高光 0/1 */
    int         shadow;        /* AE_SHADOW_* */
    int         edge;          /* 1px 边缘线 0/1 */
    int         pressable;     /* 是否参与全局按压反馈 */
};

/* 配方表只读查询；id 越界返回 NULL */
const struct aether_mat_recipe *aether_material(int id);
int aether_material_count(void);
/* 准入/嵌套判定直接复用上游 spec，避免第二套规则 */
int  aether_material_allows_glass(int id);
int  aether_material_can_refract(int id, int parent_refracting);

/* ---- 同帧模糊预算（分册 §7.3 / 第 16 章 E02） ----
 * 每帧开头 begin_frame() 清零；真算一次模糊前 request_blur() 登记并返回
 * 是否允许真算。同一帧内多个玻璃面共享一次下采样结果，第二次起返回 0，
 * 调用方应复用已有盘子而不是重算（禁止全屏 3-pass 模糊）。 */
void aether_material_begin_frame(void);
int  aether_material_request_blur(int x, int y, int w, int h, int radius);
int  aether_material_blur_budget(void);   /* 本帧真算次数，E02 应 ≤1 */

/* ---- 全局按压反馈（分册 §7.7 / 第 16 章 C01） ----
 * 外壳每帧注入触点；控件层只查询进度，不自行接线。
 * down 当帧必须可见反馈（不等待 up）；爬升 TK_D_FAST(150ms)、
 * 回落 TK_D_REVERSE(60ms)；提亮量深色 0.14 / 浅色 0.30；
 * 膨胀 +TK_PRESS_INFLATE(8px) 仅改变视觉绘制尺寸，不改变布局与命中区。 */
void  aether_material_set_touch(int x, int y, int down);
void  aether_material_tick(void);
int   aether_material_pressed(int x, int y, int w, int h);
float aether_material_press_k(int theme);   /* 0..1 进度 */
int   aether_material_press_inflate(void);   /* 当前膨胀 px */
float aether_material_press_lift(int theme); /* 当前提亮量 0..0.14/0.30 */

#endif /* AETHER_MATERIAL_H */
