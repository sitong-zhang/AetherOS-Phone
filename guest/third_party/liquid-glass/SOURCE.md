# 上游来源与许可

本目录保存 AetherOS 液态玻璃实现所照搬的两个开源项目的**原样源码副本**，用于逐行核对公式、参数与组件规范。`upstream/` 下所有文件均未修改，哈希见 `SHA256SUMS`。

## 项目一：Kyant0/AndroidLiquidGlass

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/Kyant0/AndroidLiquidGlass |
| 分支 / Commit | `kmp` / `65ab177e90e5c1d8c62e70cf7755841982da65f6`（2026-08-26） |
| 许可 | Apache-2.0（全文见 `LICENSE-Kyant0-Apache-2.0.txt`） |
| 定位 | Compose Multiplatform 液态玻璃**底层光学效果与光学基准**；仓库 README 明确声明「不含高层组件，需自行创建」 |
| 纳入内容 | `backdrop/` 效果链、AGSL shader（SDF / 折射 / 色散 / 高光 / 阴影）、`app/` 中的 LiquidButton / LiquidToggle / LiquidSlider / LiquidBottomTabs 参考组件 |

## 项目二：sdegenaar/liquid_glass_widgets

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/sdegenaar/liquid_glass_widgets |
| 分支 / Commit | `main` / `9f7b56ee061bc44be3c8b7a66c60d8378379fea3`（2026-11-07） |
| 许可 | MIT（全文见 `LICENSE-sdegenaar-MIT.txt`） |
| 定位 | Flutter 液态玻璃**全套控件组件库**：60+ Cupertino 组件、物理光学 shader、手势物理、自适应降级 |
| 纳入内容 | `shaders/`（GLSL 光学与模糊）、`lib/constants/`、`lib/types/`、`lib/src/engine/`、`lib/theme/`、`lib/utils/`、`lib/widgets/`、`docs/`、`skills/` |

### 二级上游归属

`liquid_glass_widgets` 的 `lib/src/engine/` 源自 Tim Lehmann（whynotmake.it）的
[`liquid_glass_renderer`](https://github.com/whynotmake-it/flutter_liquid_glass) v0.2.0-dev.4（MIT），
弹簧物理源自其 `motor` 包（MIT）。相关许可全文见 `LICENSE-liquid_glass_renderer-MIT.txt`，
归属清单见 `upstream/sdegenaar/liquid_glass_widgets/lib/src/engine/ATTRIBUTION.md`。
上述归属随本项目一并保留。

## 实现形态说明

两个上游均为 Kotlin / AGSL `RuntimeShader`（Compose）与 Dart / GLSL Fragment Shader（Flutter / Impeller），
依赖 GPU 与 Compose、Flutter、Skia 运行时。AetherOS 客机为自研 init + 自研图形合成器，
纯 C 逐像素渲染到 XRGB8888 framebuffer（`/dev/fb0`），不具备上述运行时，上游源码无法整段编译。

因此 AetherOS 的液态玻璃实现采取以下形态，不自行发明任何公式或参数：

1. 上游源码与许可证**原样纳入本目录**，可逐行核对（本文件与 `SHA256SUMS`）。
2. `MAPPING.md` 逐条列出「上游文件 : 符号 → AetherOS C 符号」的对应关系。
3. `guest/src/aether-glass.c` / `.h` 忠实重实现上游数学公式与参数。
4. `guest/src/aether-glass-spec.h` 把上游组件尺寸、圆角、阴影、按压、转场、层级与降级规则固化为 C 常量。

## 许可合规

- Apache-2.0 要求保留版权与许可声明、标注修改：本文件与 `MAPPING.md`、`aether-glass.h` 头部均已标注。
- MIT 要求保留版权与许可声明：三份 LICENSE 全文均随仓库分发。
- 上游源码副本保持原样，未作任何修改，避免污染上游署名。
