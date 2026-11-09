/*
 * aether-glass.h - AetherOS 液态玻璃材质（开源实现照搬，非自研算法）
 *
 * 本文件及 aether-glass.c 中的【全部数学公式、参数默认值、降级规则】均照搬自
 * 以下开源仓库，未自己发明：
 *
 *  [1] Kyant0/AndroidLiquidGlass  (Apache-2.0, commit 65ab177e)
 *      https://github.com/Kyant0/AndroidLiquidGlass
 *      - backdrop/.../internal/Shaders.kt
 *          sdRoundedRect / gradSdRoundedRect / radiusAt（SDF 圆角矩形与梯度）
 *          circleMap 折射位移、RoundedRectRefraction[WithDispersion]（7 段色散）
 *          DefaultHighlight / AmbientHighlight（法线·光照夹角高光）
 *      - backdrop/.../highlight/HighlightStyle.kt（Default: 45° / alpha .5 / falloff 1）
 *      - backdrop/.../effects/Lens.kt, Blur.kt（效果链顺序、参数语义）
 *      - app/.../components/LiquidButton.kt（lens(12f.dp, 24f.dp) 参数来源）
 *
 *  [2] sdegenaar/liquid_glass_widgets  (MIT, commit 9f7b56ee)
 *      https://github.com/sdegenaar/liquid_glass_widgets
 *      - lib/constants/glass_defaults.dart（lightAngle = 0.75π / 135°, thickness=30,
 *        blur=3, chromaticAberration=.5, refractiveIndex=1.15, lightIntensity=2）
 *      - lib/src/engine/liquid_glass_settings.dart（saturation=1.5, fresnel=1）
 *      - shaders/render.glsl（Rec.709 LUMA_WEIGHTS、applySaturation、
 *        applyGlassColor 亮度保持着色）
 *      - shaders/progressive_blur.frag（可分离高斯 + IIR 权重递推
 *        w *= g1; g1 *= g2，替代逐 tap exp()）
 *      - lib/types/glass_quality.dart（premium / standard / minimal 三档降级）
 *      - lib/constants/glass_shadow.dart、lib/utils/glass_spring.dart（见 spec 头文件）
 *      - README「Glass is a Platter, Not a Wrapper」「NEVER Nest Refractive
 *        Glass」层级规则、Accessibility Reduce Motion / High Contrast 降级
 *
 *  上游归属（随 [2] 的 THIRD_PARTY_NOTICES / ATTRIBUTION.md 一并保留）：
 *      liquid_glass_renderer (MIT, Tim Lehmann / whynotmake.it)
 *      motor                (MIT, Tim Lehmann / whynotmake.it)
 *
 * 【为什么不是"直接抄代码"】原仓库为 Kotlin / AGSL RuntimeShader 与
 * Dart / GLSL Fragment Shader，依赖 Compose、Flutter、Impeller、Skia 等 GPU
 * 运行时。Aether OS 客机按总纲第二章为「自研 init + 自研图形合成器」，
 * 纯 C 逐像素 CPU 渲染到 XRGB8888 framebuffer，不具备上述运行时，源码无法
 * 整段编译。故照搬其【数学、参数、规则】，在 C 中忠实重实现。
 *
 * 上游源码原样副本与逐条核对表：
 *      third_party/liquid-glass/upstream/   （59 文件，SHA256SUMS 可核对）
 *      third_party/liquid-glass/SOURCE.md   （版本与许可）
 *      third_party/liquid-glass/MAPPING.md  （上游符号 → C 符号逐条映射）
 */
#ifndef AETHER_GLASS_H
#define AETHER_GLASS_H

/* ---- 材质档位：照搬 glass_quality.dart 三档，并对应总纲第七章
 * 「性能不足时自动降级材质：先降模糊、再关折射、最后落实色」 ---- */
enum glass_quality {
    GLASS_MINIMAL  = 0,   /* 实色 / 零 shader 等价物（无障碍高对比亦落此档） */
    GLASS_STANDARD = 1,   /* 真算高斯模糊 + Rec.709 饱和度 + 法线高光 */
    GLASS_PREMIUM  = 2    /* 标准档 + SDF 边缘折射 + 7 段色散 */
};

void aether_glass_set_quality(int q);
int  aether_glass_quality(void);

/* ---- 光照与高光：两个不同来源的量，不再混用 ---- */
/* [S] glass_defaults.dart : lightAngle = 0.75π → 135°，iOS 26 左上光源方向 */
#define GLASS_LIGHT_ANGLE_DEG   135.0f
/* [K] HighlightStyle.kt : Default.angle = 45f，高光法线夹角 */
#define GLASS_HIGHLIGHT_ANGLE_DEG 45.0f
/* [K] HighlightStyle.kt : Default.color = White(alpha .5f), falloff = 1f */
#define GLASS_HIGHLIGHT_ALPHA   0.5f
#define GLASS_HIGHLIGHT_FALLOFF 1.0f

/* ---- [S] glass_defaults.dart 默认参数（数值由开源库给出，不自定） ---- */
#define GLASS_THICKNESS_DEFAULT 30.0f        /* thickness = 30.0 */
#define GLASS_BLUR_DEFAULT      3.0f         /* blur = 3.0（5.0 是 settings 构造默认，另一处） */
#define GLASS_LIGHT_INTENSITY   2.0f         /* lightIntensity = 2.0 */
#define GLASS_CHROMATIC_ABERRATION 0.5f      /* chromaticAberration = 0.5 */
#define GLASS_REFRACTIVE_INDEX  1.15f        /* refractiveIndex = 1.15 */

/* ---- [S] liquid_glass_settings.dart 构造默认值 ---- */
#define GLASS_SATURATION        1.5f         /* saturation = 1.5 */
#define GLASS_FRESNEL_STRENGTH  1.0f         /* fresnelStrength = 1.0 */

/* ---- 折射量：[K] app/.../LiquidButton.kt : lens(12f.dp, 24f.dp) ----
 * Lens.kt 本身是 effect 函数定义，无内建默认值；H/A 取自上游调用点，
 * 不再错误标注为「Lens.kt: 24f.dp / 48f.dp」。 */
#define GLASS_REFRACTION_H      12.0f
#define GLASS_REFRACTION_A      24.0f

/* [S] render.glsl : LUMA_WEIGHTS，ITU-R Rec.709 */
#define GLASS_LUMA_R            0.2126f
#define GLASS_LUMA_G            0.7152f
#define GLASS_LUMA_B            0.0722f

/* 组件与交互规范（尺寸/圆角/阴影/按压/转场/层级），见 MAPPING.md 第 E/F 节 */
#include "aether-glass-spec.h"

/* ---- 纯数学（可直接单元测试，不依赖画布） ---- */
/* SDF：照搬 Shaders.kt sdRoundedRect / gradSdRoundedRect */
float glass_sd_rrect(float cx, float cy, float hx, float hy, float r);
void  glass_grad_rrect(float cx, float cy, float hx, float hy, float r,
                       float *gx, float *gy);
/* 折射：照搬 circleMap + Lens.kt 的 d = circleMap(1-(-sd)/H)*A */
float glass_circle_map(float x);
float glass_refraction_d(float sd, float height, float amount);
/* 高光：照搬 DefaultHighlightShaderString：pow(abs(dot(grad,normal)),falloff) */
float glass_highlight(float gx, float gy, float angle_rad, float falloff);
/* Rec.709 亮度保持饱和度：照搬 render.glsl applySaturation */
void  glass_saturate(float *r, float *g, float *b, float saturation);
/* 亮度保持着色：照搬 render.glsl applyGlassColor（adaptive） */
void  glass_apply_glass_color(float *r, float *g, float *b,
                              float tr, float tg, float tb, float a);
/* 7 段色散权重表：照搬 RoundedRectRefractionWithDispersionShaderString */
#define GLASS_DISP_TAPS 7
struct glass_disp_tap { float offset; float wr, wg, wb; };
const struct glass_disp_tap *glass_disp_taps(void);

/* ---- 画布级合成（实现在 aether-glass.c，须由 render 层提供像素访问器） ---- */
/* 可分离高斯：照搬 progressive_blur.frag 的 IIR 递推，替换旧自造 box blur */
void aether_glass_blur_rect(int x, int y, int w, int h, int radius);
/* 完整液态玻璃浮层：模糊 → 边缘折射/色散 → 高光 → 饱和度 → 色调罩
 * solid_rgb 为 minimal 档（无障碍/性能兜底）使用的不透明实色。 */
void aether_glass_panel(int x, int y, int w, int h, int rect_radius,
                        int blur_radius, unsigned tint, unsigned char tint_a,
                        unsigned highlight_rgb, unsigned solid_rgb);
/* 模糊 + 色调罩（standard 档，保持既有 API 语义） */
void aether_glass_blur_tint(int x, int y, int w, int h, int radius,
                            unsigned tint, unsigned char a);
/* 磨砂遮罩快路径：把区域下层的「真算模糊」结果写入 dst 缓冲而不改画布。
 * 专供桌面壁纸分层（底层清晰壁纸 / 上层磨砂玻璃遮罩），避开完整合成循环
 * 在软渲染 TCG 上长时间阻塞串行 IPC 主循环。见 aether-glass.c 实现注释。 */
void aether_glass_blur_veil(unsigned char *dst, int dst_pitch,
                            int x, int y, int w, int h, int radius);

/* ---- 控件级装饰（内容层不折射，但补齐统一的圆角/阴影/镜面高光/按压） ----
 * 上游依据：[S] glass_shadow.dart 双层阴影、glass_defaults.dart
 * specularLightAlpha/specularDarkAlpha 与 ambientLight 提亮。
 * 应用内页面、灵动岛、浮层共用，保证「全系统一套控件语言」（总纲 6.2）。 */
void     aether_glass_drop_shadow(int x, int y, int w, int h, int r);
void     aether_glass_specular_edge(int x, int y, int w, int h, int r, int theme);
unsigned aether_glass_lift(unsigned rgb, float k);

#endif /* AETHER_GLASS_H */
