# 逐条照搬对照表

本表把 AetherOS 液态玻璃实现中的每一条公式、参数、规则映射到上游源码的**具体文件与符号**，供逐行核对。
上游原样副本位于 `upstream/`，精确 commit 见 `SOURCE.md`。

- `[K]` = `upstream/Kyant0/AndroidLiquidGlass/`
- `[S]` = `upstream/sdegenaar/liquid_glass_widgets/`
- `[C]` = AetherOS 客机源码 `guest/src/`

## A. SDF 与折射光学（`[C]aether-glass.c`）

| AetherOS 符号 | 上游文件 : 符号 | 照搬内容 |
|---|---|---|
| `glass_sd_rrect()` | `[K]backdrop/.../internal/Shaders.kt : sdRoundedRect` | `outside = length(max(q,0)) - r`、`inside = min(max(q.x,q.y),0)`、`return outside+inside` |
| `glass_grad_rrect()` | `[K]Shaders.kt : gradSdRoundedRect` | 角区 `sign(coord)*normalize(max(q,0))`，内部 `step(q.y,q.x)` 选轴；`gradRadius = min(r*1.5, min(hx,hy))` |
| `glass_circle_map()` | `[K]Shaders.kt : circleMap` | `1 - sqrt(1 - x²)` |
| `glass_refraction_d()` | `[K]Shaders.kt : RoundedRectRefractionShaderString : d` | `d = circleMap(1 - (-sd)/H) * A`；`-sd >= H` 时直接取原像素 |
| `k_disp[]` 7 段色散 | `[K]Shaders.kt : RoundedRectRefractionWithDispersionShaderString` | 采样偏移 `+1, +2/3, +1/3, 0, -1/3, -2/3, -1`；权重 `r/3.5`、`g/7`、`g/3.5`、`b/3.0`、`r/7` 逐通道累加 |
| 色散强度 | `[K]Shaders.kt : dispersionIntensity` + `[K]effects/Lens.kt : setFloatUniform("chromaticAberration", 1f)` | `1.0 * (cx*cy)/(hx*hy)`；Kyant0 开启色散时系数为 **1.0**，`[S]glass_defaults.dart` 的 `0.5` 属 Flutter 另一条管线，**不混用** |
| `glass_highlight()` | `[K]Shaders.kt : DefaultHighlightShaderString` | `intensity = pow(abs(dot(grad, normal)), falloff)`，`normal=(cos θ, sin θ)` |
| 效果链顺序 | `[K]DrawBackdropModifier.kt : drawBackdrop` + `[K]LiquidButton.kt : effects { vibrancy(); blur(); lens() }` | 模糊 → 折射/色散 → 高光 → 阴影 |

## B. 着色与模糊（`[C]aether-glass.c`）

| AetherOS 符号 | 上游文件 : 符号 | 照搬内容 |
|---|---|---|
| `GLASS_LUMA_R/G/B` | `[S]shaders/render.glsl : LUMA_WEIGHTS` | `(0.2126, 0.7152, 0.0722)`，ITU-R Rec.709 |
| `glass_saturate()` | `[S]shaders/render.glsl : applySaturation` | `mix(vec3(luma), color, saturation)` 亮度保持 |
| 色调罩 | `[S]shaders/render.glsl : applyGlassColor` | adaptive 模式按 chroma 权重在 directMix / luminosityMix 间插值 |
| `gaussian_axis()` | `[S]shaders/progressive_blur.frag` | `kHalf=24`、`stride=max(1, 3σ/24)`、IIR `g1=exp(-stride²·inv2s2)`、`g2=g1²`、`w*=g1; g1*=g2`；越界 tap 丢弃而非 clamp |
| 可分离两遍 | `[S]ARCHITECTURE.md : Blur pass` | 水平 + 垂直两遍构成 2D 高斯 |

## C. 参数默认值（`[C]aether-glass.h`）

| 常量 | 上游真值 | 说明 |
|---|---|---|
| `GLASS_LIGHT_ANGLE_DEG 135` | `[S]lib/constants/glass_defaults.dart : lightAngle = 0.75π` | iOS 26 左上光源，用于**光照方向** |
| `GLASS_HIGHLIGHT_ANGLE_DEG 45` | `[K]highlight/HighlightStyle.kt : Default.angle = 45f` | 高光法线夹角，**与光照角是两个不同的量** |
| `GLASS_HIGHLIGHT_ALPHA 0.5` | `[K]HighlightStyle.kt : Default.color = White(alpha 0.5f)` | Plus 混合 |
| `GLASS_HIGHLIGHT_FALLOFF 1.0` | `[K]HighlightStyle.kt : Default.falloff = 1f` | |
| `GLASS_REFRACTION_H 12` / `GLASS_REFRACTION_A 24` | `[K]app/.../components/LiquidButton.kt : lens(12f.dp, 24f.dp)` | 上游**调用点**参数，非 Lens.kt 默认值 |
| `GLASS_THICKNESS_DEFAULT 30` | `[S]glass_defaults.dart : thickness = 30.0` | |
| `GLASS_BLUR_DEFAULT 3` | `[S]glass_defaults.dart : blur = 3.0` | 5.0 是 `LiquidGlassSettings` 构造默认，属另一处 |
| `GLASS_LIGHT_INTENSITY 2.0` | `[S]glass_defaults.dart : lightIntensity = 2.0` | |
| `GLASS_CHROMATIC_ABERRATION 0.5` | `[S]glass_defaults.dart : chromaticAberration = 0.5` | |
| `GLASS_REFRACTIVE_INDEX 1.15` | `[S]glass_defaults.dart : refractiveIndex = 1.15` | |
| `GLASS_SATURATION 1.5` | `[S]lib/src/engine/liquid_glass_settings.dart : saturation = 1.5` | 构造默认值 |
| `GLASS_FRESNEL_STRENGTH 1.0` | `[S]liquid_glass_settings.dart : fresnelStrength = 1.0` | |

## D. 质量档与降级

| AetherOS 符号 | 上游文件 : 符号 | 照搬内容 |
|---|---|---|
| `enum glass_quality` 三档 | `[S]lib/types/glass_quality.dart : standard / premium / minimal` | minimal = 零自定义 shader，仅 BackdropFilter 等价实色 |
| 焦点 premium / 背景 minimal | `[S]skills/.../SKILL.md : Rule 5 Honor Quality Tiers` | 常驻导航与浮层 premium，卡片/输入 standard，密集列表 minimal |
| P75 阈值 20 / 28 ms | `[S]docs/ADAPTIVE_QUALITY.md` | `<20ms premium`、`20–28ms standard`、`>28ms minimal` |
| 降级序 | `[S]skills/.../SKILL.md` + `[S]docs/ARCHITECTURE.md` | 先降模糊 → 再关折射 → 最后落实色 |
| 无障碍 | `[S]README.md : Accessibility` | Reduce Motion 弹簧瞬时吸附；High Contrast 绕过 shader 落 minimal |

## E. 组件与交互规范（`[C]aether-glass-spec.h`）

| 常量组 | 上游文件 : 符号 | 值 |
|---|---|---|
| 圆角 | `[S]glass_defaults.dart : borderRadius / Small / Large / capsuleRadius` | 16 / 8 / 20 / 9999 |
| 内边距 | `[S]glass_defaults.dart : paddingCard / Panel / Input / Compact / Minimal` | 16 / 24 / (16,12) / 8 / 4 |
| 控件高度 | `[S]glass_defaults.dart : heightControl / heightButton / heightInput` | 32 / 48 / 48 |
| 按钮 | `[K]LiquidButton.kt : height(48.dp) / padding(16.dp) / spacedBy(8.dp)` | 48 / 16 / 8 |
| 动效时长 | `[S]glass_defaults.dart : animationDuration / Fast / Slow / materialize / dematerialize` | 200 / 100 / 300 / 250 / 350 ms |
| 按压反馈 | `[S]glass_defaults.dart : ambientBaseLight / Dark / ambientLiftDuration / Reverse` | 0.30 / 0.14 / 150 / 60 ms |
| 遮罩 | `[S]glass_defaults.dart : barrierColor` | `0x8A000000`（约 54% 黑） |
| 镜面 alpha | `[S]glass_defaults.dart : specularLightAlpha / Dark` | 0.15 / 0.10 |
| 双层阴影 | `[S]lib/constants/glass_shadow.dart : elevation / contact` | 6% 黑 / blur 8 / dy 2；2% 黑 / blur 2 / dy 1；**反向裁剪只在玻璃边界外** |
| 弹簧 | `[S]lib/utils/glass_spring.dart : bouncy / snappy / smooth / interactive` | 500ms bounce 0.3 / 0.15 / 0.0；150ms |
| 按压形变 | `[K]LiquidButton.kt : layerBlock` | `scale = lerp(1, 1+4dp/h, progress)`；`tanh(0.05·offset/max)` 跟手位移 |
| 转场 | `[S]docs/GLASS_NAVIGATION_TRANSITION.md` | materialize 淡入+高斯+微缩放，dissolve 窗口跨越转场中点 |

## F. 层级规则（总纲第六章 ↔ 上游）

| 规则 | 上游出处 | AetherOS 落点 |
|---|---|---|
| 四层：背景 → 内容 → 浮层 → 模态 | `[S]skills/.../SKILL.md : GlassScaffold` 结构图 | `enum glass_layer_role` |
| **壁纸/渐变做背景，玻璃只做导航与控制层** | `[S]README.md : Glass vs Content — Design Philosophy`；`SKILL.md : Rule 1 Glass is a Platter, Not a Wrapper` | `GLASS_ROLE_*` + 使用约束注释 |
| 内容区不透明，列表单元格/文章/媒体不用玻璃 | `[S]README.md : ❌ Keep opaque` 表 | 内容层禁止玻璃 |
| **禁止折射玻璃嵌套折射玻璃** | `[S]SKILL.md : Rule 2 NEVER Nest Refractive Glass` | `glass_role_can_refract()` 判定 |
| 背景亮度自适应 | `[S]ARCHITECTURE.md` | 暗背景 1.2× 强度、亮背景 0.8× |

## G. 原实现中的错标与混搭（本次修正）

| 位置 | 原写法 | 问题 | 修正 |
|---|---|---|---|
| `aether-glass.h` | `GLASS_REFRACTION_H 24 /* Lens.kt: 24f.dp */`、`A 48 /* Lens.kt: 48f.dp */` | `Lens.kt` 是 effect 函数定义，**不存在**这两个默认值；24f.dp 实际来自 `LiquidButton.kt` 且为 `lens(12, 24)` | 改为 `H 12 / A 24`，出处改为 `LiquidButton.kt` |
| `aether-glass.c` `glass_composite()` | 用 `GLASS_LIGHT_ANGLE_DEG(135)` 计算 Kyant0 高光 | 把 sdegenaar 的光照角塞进了 Kyant0 的 45° 高光公式 | 分离 `GLASS_HIGHLIGHT_ANGLE_DEG 45` 与 `GLASS_LIGHT_ANGLE_DEG 135` |
| `aether-glass.h` | `GLASS_BLUR_DEFAULT 5.0 /* glass_defaults.dart */` | `glass_defaults.dart` 中 `blur = 3.0`；5.0 是 `LiquidGlassSettings` 构造默认 | 改为 3.0 并注明两处来源 |
| `aether-glass.h` | 参数出处写成「`glass_defaults.dart` 全部默认」 | 实际分属两个文件：`glass_defaults.dart`（thickness/blur/CA/index/intensity）与 `liquid_glass_settings.dart`（saturation/fresnel） | 逐常量标注真实文件 |
| `aether-glass.c` `glass_composite()` | 色散 `(cx*cy)/(hx*hy)` | 核对后**并非缺陷**：Kyant0 `Lens.kt` 开启色散时 `chromaticAberration=1f`，现有写法等价；若误乘 sdegenaar 的 0.5 反而造成新混搭 | 保持不变，在对照表中写明 |
| `aether-glass.c` 注释 | 「亮度保持着色」 | 实现为普通 alpha 混合，**注释与实现不符** | 补 `glass_apply_glass_color()` 实现 adaptive 亮度保持 |
| 全工程 | 组件尺寸/圆角/阴影/按压/转场为各处硬编码自定 | 上游已有完整规范却未采用 | 新增 `aether-glass-spec.h` 统一固化 |
