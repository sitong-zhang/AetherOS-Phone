/*
 * aether-glass.h - AetherOS 液态玻璃材质（开源实现照搬，非自研算法）
 *
 * 本文件及 aether-glass.c 中的【全部数学公式、参数默认值、降级规则】均照搬自
 * 以下开源仓库，未自己发明：
 *
 *  [1] Kyant0/AndroidLiquidGlass  (Apache-2.0)
 *      https://github.com/Kyant0/AndroidLiquidGlass
 *      - backdrop/.../internal/Shaders.kt
 *          sdRoundedRect / gradSdRoundedRect / radiusAt（SDF 圆角矩形与梯度）
 *          circleMap 折射位移、RoundedRectRefraction[WithDispersion]（7 段色散）
 *          DefaultHighlight / AmbientHighlight（法线·光照夹角高光）
 *      - backdrop/.../effects/Lens.kt, Blur.kt（效果链顺序、参数语义）
 *
 *  [2] sdegenaar/liquid_glass_widgets  (MIT, v1.17.0)
 *      https://github.com/sdegenaar/liquid_glass_widgets
 *      - lib/constants/glass_defaults.dart（lightAngle = 0.75π / 135°, thickness,
 *        blur, refraction, chromaticAberration, saturation 等默认值）
 *      - shaders/render.glsl（Rec.709 LUMA_WEIGHTS、applySaturation、
 *        applyGlassColor 亮度保持着色）
 *      - shaders/progressive_blur.frag（可分离高斯 + IIR 权重递推
 *        w *= g1; g1 *= g2，替代逐 tap exp()）
 *      - lib/types/glass_quality.dart（premium / standard / minimal 三档降级）
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

/* ---- 照搬自 glass_defaults.dart 的默认参数（数值由开源库给出，不自定） ---- */
#define GLASS_LIGHT_ANGLE_DEG   135.0f       /* Apple iOS 26 标准，左上光源 */
#define GLASS_THICKNESS_DEFAULT 30.0f
#define GLASS_BLUR_DEFAULT      5.0f
#define GLASS_SATURATION        1.5f
#define GLASS_REFRACTION_H      24.0f        /* Lens.kt: 24f.dp */
#define GLASS_REFRACTION_A      48.0f        /* Lens.kt: 48f.dp */
#define GLASS_HIGHLIGHT_FALLOFF 1.0f
#define GLASS_LUMA_R            0.2126f      /* ITU-R Rec.709 */
#define GLASS_LUMA_G            0.7152f
#define GLASS_LUMA_B            0.0722f

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

#endif /* AETHER_GLASS_H */
