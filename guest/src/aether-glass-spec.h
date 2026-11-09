/*
 * aether-glass-spec.h - AetherOS 液态玻璃【组件与交互规范】
 *
 * 本文件所有数值均逐条照搬自开源项目，不自行设定。上游源码原样副本位于
 * third_party/liquid-glass/upstream/，逐条对应关系见 MAPPING.md 第 E/F 节。
 *
 *  [S] sdegenaar/liquid_glass_widgets (MIT, 9f7b56ee)
 *      lib/constants/glass_defaults.dart            圆角/内边距/高度/时长/按压/遮罩/镜面
 *      lib/constants/glass_shadow.dart              双层阴影
 *      lib/utils/glass_spring.dart                  弹簧曲线
 *      lib/types/glass_quality.dart                 三档质量
 *      docs/ADAPTIVE_QUALITY.md                     P75 自适应阈值
 *      docs/GLASS_NAVIGATION_TRANSITION.md          materialize 转场
 *      README.md / skills/.../SKILL.md             四层结构与使用规则
 *  [K] Kyant0/AndroidLiquidGlass (Apache-2.0, 65ab177e)
 *      app/.../components/LiquidButton.kt          按钮尺寸与按压形变
 *
 * 规则摘要（上游硬性约束，AetherOS 同样遵守）：
 *   - 玻璃只用于「导航 / 控制层」浮层，壁纸与内容层不透明；
 *   - 禁止折射玻璃嵌套折射玻璃；
 *   - 阴影反向裁剪，只落在玻璃边界之外；
 *   - 焦点元素 premium、背景/密集列表 minimal。
 */
#ifndef AETHER_GLASS_SPEC_H
#define AETHER_GLASS_SPEC_H

/* ==================== 四层结构（[S] SKILL.md GlassScaffold / 总纲 6.1） ====================
 * 背景 → 内容 → 浮层 → 模态，四层不混淆。
 * 壁纸/渐变属背景层；列表、卡片、文章、媒体属内容层，保持不透明。 */
enum glass_layer_role {
    GLASS_ROLE_BACKGROUND = 0,  /* 壁纸 / 渐变：禁止玻璃 */
    GLASS_ROLE_CONTENT    = 1,  /* 内容：不透明，禁止玻璃 */
    GLASS_ROLE_NAV        = 2,  /* 导航/控制浮层：允许玻璃（状态栏、Dock、标签栏） */
    GLASS_ROLE_OVERLAY    = 3,  /* 浮层/模态：允许玻璃（通知中心、控制中心、弹窗） */
    GLASS_ROLE_MODAL      = 4   /* 模态：允许玻璃 + 遮罩 */
};

/* 玻璃使用约束：[S] README「Glass vs Content」+ SKILL.md Rule 1/2 */
/* 只有导航层与浮层/模态允许折射玻璃；背景与内容层一律不透明。 */
static inline int glass_role_allows_glass(int role)
{
    return role >= GLASS_ROLE_NAV;
}
/* Rule 2：折射玻璃内部不得再嵌套折射玻璃。
 * parent_refracting = 父容器是否已经是一块 premium 折射玻璃。 */
static inline int glass_role_can_refract(int role, int parent_refracting)
{
    return glass_role_allows_glass(role) && !parent_refracting;
}

/* ==================== 圆角（[S] glass_defaults.dart） ==================== */
#define GLASS_RADIUS            16.0f   /* borderRadius */
#define GLASS_RADIUS_SMALL      8.0f   /* borderRadiusSmall */
#define GLASS_RADIUS_LARGE     20.0f   /* borderRadiusLarge */
#define GLASS_RADIUS_CAPSULE 9999.0f   /* capsuleRadius：任意高度均为真胶囊 */
#define GLASS_RADIUS_MAX    99999.0f   /* maxSafeRadius：防 infinity 塌陷 */

/* ==================== 内边距（[S] glass_defaults.dart） ==================== */
#define GLASS_PAD_CARD_H        16.0f   /* paddingCard */
#define GLASS_PAD_CARD_V        16.0f
#define GLASS_PAD_PANEL         24.0f   /* paddingPanel */
#define GLASS_PAD_INPUT_H       16.0f   /* paddingInput horizontal */
#define GLASS_PAD_INPUT_V       12.0f   /* paddingInput vertical */
#define GLASS_PAD_COMPACT        8.0f   /* paddingCompact */
#define GLASS_PAD_MINIMAL        4.0f   /* paddingMinimal */

/* ==================== 控件高度（[S] glass_defaults.dart / [K] LiquidButton.kt） ====== */
#define GLASS_H_CONTROL         32.0f   /* heightControl */
#define GLASS_H_BUTTON          48.0f   /* heightButton（LiquidButton.kt: .height(48f.dp)） */
#define GLASS_H_INPUT           48.0f   /* heightInput */
#define GLASS_BTN_PAD_H         16.0f   /* LiquidButton.kt: .padding(horizontal = 16f.dp) */
#define GLASS_BTN_GAP            8.0f   /* LiquidButton.kt: Arrangement.spacedBy(8f.dp) */

/* ==================== 动效时长（[S] glass_defaults.dart，单位 ms） ==================== */
#define GLASS_ANIM_MS           200     /* animationDuration */
#define GLASS_ANIM_FAST_MS      100     /* animationDurationFast */
#define GLASS_ANIM_SLOW_MS      300     /* animationDurationSlow */
#define GLASS_MATERIALIZE_MS    250     /* materializeDuration：淡入 250ms */
#define GLASS_DEMATERIALIZE_MS  350     /* dematerializeDuration：淡出 350ms */
#define GLASS_SPRING_MS         500     /* glass_spring.dart bouncy/snappy/smooth */
#define GLASS_SPRING_INTERACTIVE_MS 150 /* glass_spring.dart interactive */

/* 弹簧 bounce 系数（[S] glass_spring.dart） */
#define GLASS_SPRING_BOUNCE_BOUNCY  0.30f
#define GLASS_SPRING_BOUNCE_SNAPPY  0.15f
#define GLASS_SPRING_BOUNCE_SMOOTH  0.00f

/* ==================== 按压反馈（[S] glass_defaults.dart） ====================
 * 按下时表面整体提亮：浅色 +0.30 luma，深色 +0.14 luma；
 * 提亮随按压膨胀 150ms 爬升、60ms 回落。 */
#define GLASS_AMBIENT_LIGHT_L       0.30f
#define GLASS_AMBIENT_LIGHT_D       0.14f
#define GLASS_AMBIENT_LIFT_MS       150
#define GLASS_AMBIENT_REVERSE_MS     60
/* [K] LiquidButton.kt layerBlock：scale = lerp(1, 1 + 4dp/h, progress) */
#define GLASS_PRESS_INFLATE_DP      4.0f
/* [K] LiquidButton.kt：tanh(0.05 * offset / max) 跟手位移 */
#define GLASS_DRAG_INIT_DERIV       0.05f
/* [S] GlassButton：可选固定 15% 交互缩放 */
#define GLASS_PRESS_SCALE_ALT       1.15f

/* ==================== 遮罩与镜面（[S] glass_defaults.dart） ==================== */
#define GLASS_BARRIER_ARGB      0x8A000000u  /* barrierColor：约 54% 黑，模态背后压暗 */
#define GLASS_SPECULAR_LIGHT    0.15f        /* specularLightAlpha */
#define GLASS_SPECULAR_DARK     0.10f        /* specularDarkAlpha */

/* ==================== 双层阴影（[S] glass_shadow.dart） ====================
 * elevation：约 6% 黑、8px 模糊、y 偏移 2；
 * contact  ：约 2% 黑、2px 模糊、y 偏移 1。
 * 二者反向裁剪，只出现在玻璃边界之外，避免玻璃模糊自身阴影。 */
#define GLASS_SHADOW_ELEV_ARGB   0x0F000000u
#define GLASS_SHADOW_ELEV_BLUR      8
#define GLASS_SHADOW_ELEV_DY        2
#define GLASS_SHADOW_CONTACT_ARGB 0x05000000u
#define GLASS_SHADOW_CONTACT_BLUR   2
#define GLASS_SHADOW_CONTACT_DY     1

/* ==================== 质量档自适应（[S] docs/ADAPTIVE_QUALITY.md） ========== */
#define GLASS_P75_PREMIUM_MS      20.0f  /* P75 < 20ms → premium */
#define GLASS_P75_STANDARD_MS     28.0f  /* 20–28ms → standard；>28ms → minimal */
/* 背景亮度自适应（[S] ARCHITECTURE.md）：暗背景玻璃更实 1.2×，亮背景更透 0.8× */
#define GLASS_ADAPTIVE_DARK      1.20f
#define GLASS_ADAPTIVE_LIGHT     0.80f

/* ==================== 无障碍（[S] README Accessibility） ====================
 * Reduce Motion：所有弹簧/果冻动画瞬时吸附到目标值；
 * High Contrast：绕过全部自定义 shader，落 minimal 实色 frost 面板。 */
#define GLASS_REDUCE_MOTION_INSTANT 1
#define GLASS_HIGH_CONTRAST_MINIMAL 1

#endif /* AETHER_GLASS_SPEC_H */
