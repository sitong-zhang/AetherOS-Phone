# 第三方开源资产与许可合规

本文件列出 AetherOS 采用的第三方开源资产、来源、许可证及其在仓库内的落地位置。
上游源码副本、许可证全文与逐符号映射随仓库分发，可供独立核验。

## 采用原则

优先采用成熟开源实现，不重复发明基础能力。跨渲染栈移植时保留原作者署名与许可证，
以 C 等价重实现光学公式、参数体系、组件规范与状态机语义；上游源码文件原样留存，
不作修改，避免污染上游署名。

AetherOS 用户态为纯 C 实现，采用自研 init（PID 1）与自研图形合成器，逐像素渲染到
framebuffer，不包含 Compose、Flutter、Skia 或 GLSL 运行时。上游 Kotlin / Dart /
AGSL / GLSL 源码因此不参与产品二进制编译，仅作为规格与许可依据。

## 资产清单

### 液态玻璃光学与组件规范

| 上游 | 许可证 | 落地位置 |
|---|---|---|
| `Kyant0/AndroidLiquidGlass`（kmp 分支） | Apache-2.0 | `guest/src/aether-glass.c` 的圆角矩形 SDF、`circleMap` 折射位移、色散权重与高光公式；上游副本位于 `guest/third_party/liquid-glass/upstream/Kyant0/AndroidLiquidGlass/` |
| `sdegenaar/liquid_glass_widgets`（main 分支） | MIT | `guest/src/aether-glass.c`、`guest/src/aether-glass.h`、`guest/src/aether-glass-spec.h` 的参数体系、三档质量降级、组件与层级规则；上游副本位于 `guest/third_party/liquid-glass/upstream/sdegenaar/liquid_glass_widgets/` |
| `whynotmake-it/flutter_liquid_glass` → `liquid_glass_renderer` | MIT | 基础 fragment shader 管线归属，随 `guest/third_party/liquid-glass/upstream/sdegenaar/liquid_glass_widgets/lib/src/engine/ATTRIBUTION.md` 保留 |
| `whynotmake-it/rivership` → `motor` | MIT | CupertinoMotion 弹簧物理归属 |

许可证全文见 `guest/third_party/liquid-glass/LICENSE-Kyant0-Apache-2.0.txt`、
`LICENSE-sdegenaar-MIT.txt`、`LICENSE-liquid_glass_renderer-MIT.txt`；
逐符号映射见 `guest/third_party/liquid-glass/MAPPING.md`，版本说明与完整性校验见
`SOURCE.md` 与 `SHA256SUMS`。

### 灵动岛与实时活动规则

| 上游 | 许可证 | 采用内容 |
|---|---|---|
| `istornz/flutter_live_activities` | MIT | 收起态 / 展开态 / 卡片态分层与区域语义 |
| `agupta07505/SmartIsland` | GPL-3.0 | 场景清单、主副活动并存、优先级排序与自动收起状态机语义 |

实现位于 `guest/src/aether-island.c` 与 `guest/src/aether-island.h`。GPL-3.0 项目仅参考
设计规则与状态机语义，不含其代码。许可证全文见
`guest/THIRD_PARTY/LICENSE-flutter_live_activities-MIT.txt` 与
`guest/THIRD_PARTY/LICENSE-SmartIsland-GPL-3.0.txt`。

### 图标、字体与字库

| 资产 | 来源 | 许可证 | 落地位置 |
|---|---|---|---|
| 线性图标集 | Lucide `lucide-static@0.468.0` | ISC | `guest/assets/*.svg`，由 `guest/tools/gen_icons.py` 光栅化为 `guest/src/aether-icons-data.h` |
| 矢量字体 | Noto Sans CJK SC（CFF → TrueType glyf 子集） | SIL OFL 1.1 | `guest/assets/aether-font.ttf`，由 `guest/tools/gen_vector_font.py` 生成；许可全文 `guest/assets/OFL.txt` |
| CJK 点阵字库 | Noto Sans CJK SC（16px） | SIL OFL | `guest/src/aether-font-cjk.h`，由 `guest/tools/gen_font_cjk.py` 生成 |
| ASCII 点阵 | DejaVu Sans Mono 8x16 | Bitstream Vera / Public Domain | `guest/src/aether-font.h` |
| TrueType 栅格化 | `stb_truetype.h` | Public Domain / MIT | `guest/src/aether-vfont.c` |
| 品牌标识 | 自绘矢量 | 自有 | `guest/assets/` |

### 安卓兼容层

| 上游 | 许可证 | 落地位置 |
|---|---|---|
| Waydroid `tools/helpers/lxc.py` / `drivers.py` | GPL-3.0 | `guest/src/aether-android.c` 的 `unshare` / `mount` / `chroot` / `execve` 容器启动序列 |
| AOSP Virtualization | Apache-2.0 | `guest/src/aether-android-bridge.c` 的 AVF / pKVM 能力检测 |
| Linux 内核 UAPI `linux/uinput.h` | GPL-2.0 WITH Linux-syscall-note | `guest/src/aether-android-bridge.c` 的输入桥 |

上游副本、逐符号映射与校验和见 `guest/third_party/android-compat/`。

## 能力边界

安卓兼容层的 AVF 图形路径需要受限系统权限，且不具备完整图形 HAL。
`guest/src/aether-android-bridge.c` 在 pKVM 不可用或未安装兼容运行时镜像时写入
不可用状态与降级提示，应用商店的安卓应用分类相应灰显安装入口，AetherOS 本体功能不受影响。

## 构建期断言

`guest/tools/test_glass_math.c` 覆盖 SDF、折射位移、高光角分离、参数出处、色散权重与
组件常量；`guest/tools/test_android_compat.c` 覆盖状态枚举、共享帧协议常量与 Linux UAPI
事件常量。两者作为构建期可复现断言随源码分发。
