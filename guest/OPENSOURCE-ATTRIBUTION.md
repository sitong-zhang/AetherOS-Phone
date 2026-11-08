# Aether OS 开源采用说明（总纲第三章第 3 条 · 四方向逐一调研与采用理由）

> 本文件是工程合规文档，不是逐轮报告。规定「先搜后用、有成熟开源实现直接照搬、不自己发明轮子」的落地证据。
> 所有被照搬的项目，其「怎么用」与「怎么实现」均已完整调研，而非只取算法片段。

## 为什么是「照搬原理」而不是「整段复制代码」

总纲第二章钦定系统性质为「**自研 init（PID 1）+ 自研图形合成器**，不是 Android」，第五章/第十一章把「合成器怎么画、模糊怎么算、动效怎么做」交给执行方自定。当前客机是纯 C 逐像素 CPU 渲染到 XRGB8888 framebuffer，运行在 QEMU 内，**不存在** Compose、Flutter、Skia、Impeller、AGSL RuntimeShader 或 GLSL 的运行时。

因此，Kotlin / Dart / GLSL 源码无法整段编译进客机二进制。总纲第 5 行要求照搬的是「**所有技术原理与现成实现**」——即光学公式、参数体系、降级规则、组件与层级规则。工程采用的方式是：把这些公式与规则以 C 忠实重实现，并在源码头逐函数标注来源。这不是自研算法，是跨渲染栈的等价移植。

---

## 方向一 · 液态玻璃光学标杆（Compose 系，Backdrop 原版）

| 项 | 内容 |
|---|---|
| 仓库 | `Kyant0/AndroidLiquidGlass`（用户指定） |
| 检索确认 | `kmp` 分支，Apache-2.0，`io.github.kyant0:backdrop:2.0.1`，minSdk 21，Compose Multiplatform（Android/desktop/iOS/JS/WASM） |
| 完整调研范围 | README、`backdrop/build.gradle.kts`、库本体 30 个 Kotlin 文件、catalog 全部示例场景（ControlCenter / LockScreen / ProgressiveBlur / Magnifier / Home / Dialog 等）、`effects/{Blur,Lens,RenderEffect}.kt`、`internal/Shaders.kt` 全部 AGSL、`highlight/Highlight*.kt`、`DrawBackdropModifier.kt` / `BackdropEffectScope.kt` 捕获与效果链、LICENSE |
| 关键理解（怎么用） | `Modifier.drawBackdrop(backdrop, shape, effects { vibrancy(); blur(); lens(...) }, highlight)`；库**不含**高层组件，需自行组合；效果链顺序 blur→lens；backdrop 由 `LayerBackdrop` 记录图层，`RenderEffect`/`RuntimeShader` 施加 |
| 许可证 | Apache License 2.0 |
| **采用理由** | 提供业内公认的液态玻璃**光学数学基准**：圆角矩形 SDF 与梯度、`circleMap` 折射位移、7 段色散权重、法线·光照夹角高光。这些是「玻璃边缘为什么会有拉伸和反光」的权威公式，属于纯 C 软件渲染需要补齐的部分 |
| 照落到哪里 | `src/aether-glass.c`：`glass_sd_rrect` / `glass_grad_rrect` / `glass_circle_map` / `glass_refraction_d` / `glass_disp_taps` / `glass_highlight`，逐函数标注 `Shaders.kt` 来源 |

## 方向二 · 液态玻璃组件化实现（Flutter 系，完整组件库）

| 项 | 内容 |
|---|---|
| 仓库 | `sdegenaar/liquid_glass_widgets`（用户指定） |
| 检索确认 | `main` 分支，MIT，v1.17.0，601 文件 / 453 Dart / 9 个 GLSL-FRAG shader / 60+ 组件，Flutter ≥3.41 |
| 完整调研范围 | README 全 1128 行（安装、Quick Start、组件清单、设计哲学、三档质量、平台矩阵、性能、无障碍、架构、已知限制、致谢）、`pubspec.yaml`、`lib/constants/glass_defaults.dart`、`lib/src/engine/liquid_glass_settings.dart`、`lib/types/glass_quality.dart`、`shaders/{render,progressive_blur,sdf,displacement_encoding,gles_compat}.glsl`、`skills/liquid-glass-widgets/SKILL.md`（官方 AI 用法规则）、`THIRD_PARTY_NOTICES`、`lib/src/engine/ATTRIBUTION.md`、LICENSE |
| 关键理解（怎么用） | `initialize()` 预热 → `wrap()` 全局 → `GlassScaffold` 承载；**黄金规则**：Glass is a Platter not a Wrapper（玻璃只用于导航/控制层，内容区不透明）、禁止折射玻璃嵌套折射玻璃、三档质量按场景分配 |
| 参数体系（照搬数值，不自定） | `lightAngle = 0.75π / 135°`（iOS 26 标准左上光源）、`thickness 30`、`blur 5/3`、`saturation 1.5`、`chromaticAberration 0.01/0.5`、`refractiveIndex 1.2/1.15`、`falloff 1`、White alpha .5 Plus 混合 |
| 许可证 | MIT（Copyright 2024–2026 Sebastian Degenaar） |
| **采用理由** | 提供**参数体系、渲染管线分层、三档质量降级、无障碍降级、组件与层级规则**——这些决定「什么时候该用玻璃、性能不够先降哪一级」，与总纲第七章「先降模糊、再关折射、最后落实色」和 5.20 无障碍完全对应 |
| 照落到哪里 | `src/aether-glass.c`：`GLASS_*` 参数宏、Rec.709 `glass_saturate`、可分离高斯 IIR 递推（`w *= g1; g1 *= g2`，照搬 `progressive_blur.frag`）、三档 `enum glass_quality`；`src/aether-phone.c` 的 `draw_overlay_backdrop` 按 fx 档位分流 |

### 上游归属链（随 MIT 项目一并保留）

| 上游 | 许可证 | 归属 |
|---|---|---|
| `whynotmake-it/flutter_liquid_glass` → `liquid_glass_renderer` v0.2.0-dev.4 | MIT | Tim Lehmann / whynotmake.it，基础 fragment shader 管线 |
| `whynotmake-it/rivership` → `motor` v1.1.0 | MIT | Tim Lehmann / whynotmake.it，CupertinoMotion 弹簧物理 |

## 方向三 · 移动端虚拟化前端参考（Limbo 系）

| 项 | 内容 |
|---|---|
| 检索确认 | 已反编译产物 `/root/aether-archive/limbo-decompiled`，`org.libsdl.app` glue（SDLActivity / SDLSurface / `onNativeResize`） |
| **采用理由** | 只参考 SDL2 Android 的 Surface 生命周期与 native 尺寸传递链路，用于 APK 侧把真实屏幕尺寸交给引擎（1.0.11 的 `AETHER_SCREEN` 注入、`sdl2_get_present_rect` contain 缩放） |
| 边界 | 总纲第三章第 7 条、第八章「工程独立」：业务层为独立新工程 `dev.aether.vm`，不沿用第三方成品的包名、目录结构、皮肤、代码组织 |

## 方向四 · 模拟引擎来源（QEMU 系）

| 项 | 内容 |
|---|---|
| 检索确认 | QEMU 8.2.0 源码（`/root/aether-archive/aarch64-engine/qemu-8.2.0`），交叉编译 `libqemu-system-aarch64.so` |
| **采用理由** | 提供 ARM64 虚拟化、virtio-gpu / fbdev 显示后端；此处只做集成与用户态显示补丁（`ui/sdl2.c`、`ui/sdl2-2d.c`），不重复造内核能力（总纲第三章第 8 条、第十一章底线 4） |

---

## 其他开源资产

| 资产 | 来源 | 许可证 | 用途 |
|---|---|---|---|
| 76 枚线性图标 | Lucide `lucide-static@0.468.0` | ISC | 应用/状态栏/控制中心/设置统一描边图标；原始 SVG 存于 `assets/`，`tools/gen_icons.py` 离线光栅化为 `aether-icons-data.h` |
| 客机矢量字体 `assets/aether-font.ttf` | Noto Sans CJK SC（TTC index 2）修改版 | SIL OFL 1.1 | 阶段 6 起的主用字体；子集化 + CFF→`glyf` 转换 + 更名 `Aether Sans CJK`，详见下文「阶段 6」 |
| 矢量栅格化 `src/stb_truetype.h` | `stb_truetype` v1.08（Sean Barrett / RAD Game Tools） | Public Domain | 静态链入合成器，输出灰度抗锯齿字形位图 |
| 字体生成工具链 | fontTools（`subset` / `Cu2QuPen` / `TTGlyphPen`） | MIT | 仅离线生成字体资产，不进入客机运行时；脚本 `tools/gen_vector_font.py` |
| CJK 点阵字库 | Noto Sans CJK SC（TTC index 2, 16px） | SIL OFL | `tools/gen_font_cjk.py` 生成 `aether-font-cjk.h`（1001 字）；仅作矢量字体资产缺失时的**点阵回退后端** |
| ASCII 点阵 | DejaVu Sans Mono 8x16 | Bitstream Vera / Public Domain | `aether-font.h`；同上，回退后端 |
| 品牌 Logo | 自绘矢量（环形核心 + 数据通道） | 自有 | 总纲 6.5 唯一例外 |
| 系统图标 | gilbarbara/logos（Tux） | — | 旧版兼容图元 |

## 违规整改记录

- **问题**：上一阶段未先调研上述两个仓库、未给出采用理由，即自行实现降采样 box blur（放大阶段定点插值产生彩色条纹花屏），违反「开源优先」「不自己发明轮子」。
- **整改**：删除 `aether-render.c` 中自研 `fx_box_pass` / `aether_blur_rect` 的整段实现，改为薄转发；新增 `aether-glass.c`（照搬两仓库光学公式与参数）；`aether_blur_rect` / `aether_blur_tint` 转发到照搬实现。
- **验证**：三档（premium / standard / minimal）截图，列间跳变 0.5–0.8（无高频彩条），premium 背景为真算虚化 + 边缘折射亮边 + 135° 高光，minimal 为干净实色。

---

## 阶段 4 追加调研 · 灵动岛与实时活动（开源优先，先搜后用）

按总纲「任何能力先检索开源仓库，有成熟实现直接照搬」，阶段 4 开工前先做开源检索，
再完整 clone 两个最贴合《总纲》5.7 的仓库并从头到尾阅读（不只是核心算法源码）：

| 仓库 | 许可证 | 完整调研范围 | 采用理由 |
|---|---|---|---|
| `istornz/flutter_live_activities` | MIT | 完整 clone 168 文件；README 全 922 行、LICENSE、CHANGELOG、`example/ios/extension-example/extension_example.swift` 三态定义、Android RemoteViews 章节 | 提供**三种形态**的权威分层：`compactLeading` / `compactTrailing` / `minimal` / `DynamicIslandExpandedRegion(.leading/.center/.trailing)`。总纲 5.7 的收起态 / 展开态 / 卡片态与之直接对应，故照搬其形态分层与区域语义 |
| `agupta07505/SmartIsland` | GPL-3.0 | 完整 clone 633 文件；README 全 176 行、LICENSE、PRIVACY.md、SECURITY.md、ROADMAP.md、5W2H.md、`model/IslandEvent.kt`（约 30 场景分组）、`model/IslandPriority.kt`（rank / popsUp / isLive / canIdleOut / 稳定排序）、`model/IslandMode.kt`、`model/ExpandMotion.kt`、`IdleWidget.kt` 等 | 提供**场景清单、主活动+副活动并存、优先级排序、短暂事件 popsUp、自动收起、隐私点点击查应用**的完整状态机语义；`ExpandMotion.kt` 给出「宽度先拉、高度后撑」的分段进度参数 |

**照搬的具体规则（非源码）**：

- 形态分层：收起态一个小胶囊（图标 + 极短文字）、展开态横向拉长再纵向撑开、卡片态含可点控制按钮。
- 连续形变分段：`PILL_OUT_END_PROGRESS = 0.25`（宽度先拉满）、`CARD_IN_START_PROGRESS = 0.1`、`CARD_IN_END_PROGRESS = 0.9`（高度后撑）、`MIN_FADE_MS = 50`（内容淡入不短于数帧）。实现于 `aether-island.c` 的 `form_geom()` / `aether_island_render()`。
- 主副位机制：一个主活动 + 一个副活动（副收缩成侧边小圆，点击可互换）；`popsUp` 短事件在无实时活动时占主位、有实时活动时退副位；`canIdleOut` 自动收起；稳定排序（同 rank 保持到达顺序）。
- 优先级顺序**以《总纲》5.7 为准**（通话 > 录音 > 导航 > 计时器 > 音乐 > 下载 > 投屏 > 蓝牙），不照搬上游 rank 数值，因为上游把录音与计时器归在同一档。
- 隐私联动：相机/麦克风绿点、定位橙点，与状态栏同色同义；点击显示「哪个应用在用」并可直接关闭（`PRIVACY.md` 的 privacy indicator app inspector 语义）。

**边界与合规**：客机为纯 C 软件光栅化，不具备 Compose / ActivityKit / SwiftUI / Dart 运行时，
Kotlin / Dart / Swift 源码无法整段编译进客机二进制。本工程**未拷贝任何上游源码文件**，
只等价重实现上述状态机、层级规则与动效参数，并在 `THIRD_PARTY/` 保留两份原始许可证。
GPL-3.0 项目的设计规则与状态机语义仅作参考，不含其代码，故不构成其衍生作品。

**验证**：ARM64 客机真实开机复跑导出 27 帧，灵动岛五场景（音乐 / 计时器 / 录音 / 充电 / 导航）
全部现场跑通（`SELFTEST_ISLAND_5=1`），三态、主副并存与互换、隐私面板、全局搜索、负一屏
均有深浅两套真实帧，`SELFTEST_STAGE4_DONE=1`，全程无 watchdog 心跳超时。

## 阶段 5 追加说明 · 应用商店 / 多任务小窗 / 息屏显示 AOD

阶段 5 在既有骨架上做加法，**未引入任何新的第三方代码库或设计资源**：

- **图标**：应用商店（`AE_ICON_DOWNLOAD`）、笔记（`AE_ICON_TYPE`）仍取自既有 Lucide `lucide-static@0.468.0`（ISC）图标集，由 `tools/gen_icons.py` 同一管线光栅化编入 `aether-icons-data.h`，全系统仍一套描边体系（24×24 / stroke 2 / round cap）。
- **字体**：阶段 5 时笔记正文与商店文案沿用 `aether-font-cjk.h`（Noto Sans CJK SC / SIL OFL）、仍为 16×16 点阵；该限制已在阶段 6 由内嵌矢量字体替换解决（见下节）。
- **应用商店数据**：商店为**本机自策展市场**（`aether-store.c` 内 `struct store_app` 目录），**未接入 F-Droid / Aurora Store / 任何外部仓库**，不联网。诚实边界：应用**评分、下载量、推荐位**为本地策展值；应用**名称、版本号（读 `/etc/aether/version`）、安装体积、权限清单**为真实读取；安装流（下载→校验→安装→完成）按单调时钟真实推进，完成后真写桌面/抽屉空槽并触发首开权限授予弹窗，可真实打开与卸载。
- **多任务小窗**：悬浮窗为客机 WM 原生能力（同时仅一个，可拖拽移动/缩放，第二请求给 toast 反馈），无第三方依赖。
- **息屏显示 AOD**：息屏后中心时间/日期/通知数/电量极低亮度暗显，由系统设置总线 `SYS_AOD=20` 控制，关闭后息屏全黑；电源键/点击唤醒。无第三方依赖。

**验证**：host 合成器（headless 720×1280）IPC 驱动导出深浅两套共 56 帧真实 PNG，`aether_desktop_state` JSON 断言：
安装闭环使 `user_n` 由 0 变 1、`kinds:[15]`（笔记）落位桌面与 Dock；小窗第二请求返回 `float:1` 不变（单浮窗约束）；
AOD 开启帧非黑率 1.0%（极低亮度暗显）、关闭 `SYS_AOD=20` 后息屏帧 0.0%（全黑）。阶段 6 系统盘 v6 内置 `phonetest-v6.sh` 在客机复跑同口径断言。

---

## 阶段 6 追加说明 · 矢量字体与应用内更新

### 一、矢量字体（替换点阵后端）

总纲 5.20 要求「字体可缩放且布局必须重排，不许裁切重叠」，6.6 要求「字体开源可商用且中文完整覆盖」。原 8×16 / 16×16 点阵只收录源码中静态出现的 1001 个非 ASCII 字符，运行时动态文本会缺字形，且最近邻缩放产生锯齿，无法满足上述条款。按「开源优先、有成熟实现直接照搬」，直接采用成熟字库与成熟栅格化库，不自行设计字形轮廓或光栅化算法。

| 项 | 内容 |
|---|---|
| 字库来源 | Noto Sans CJK SC（`NotoSansCJK-Regular.ttc` index 2），SIL Open Font License 1.1 |
| 修改内容 | 子集化（GB2312 全量汉字 6763 + 常用符号 + 源码字符，共 7720 码点）；CFF/PostScript 三次曲线转换为 TrueType `glyf` 二次曲线；按 OFL 第 3 条改用独立字体名 `Aether Sans CJK` |
| 生成工具 | fontTools（`fontTools.subset`、`Cu2QuPen`、`TTGlyphPen`），MIT。脚本 `guest/tools/gen_vector_font.py`，产物 `guest/assets/aether-font.ttf`（2,080,884 B） |
| 栅格化 | `stb_truetype.h` v1.08（Sean Barrett / RAD Game Tools，public domain），静态链入客机合成器；`stbtt_ScaleForPixelHeight` / `GetCodepointBitmapBox` / `MakeCodepointBitmap` 输出灰度抗锯齿位图 |
| 完整许可证 | `guest/assets/OFL.txt`：含原版权声明、修改说明、生成工具与栅格化工具归属 |
| **采用理由** | 中文完整覆盖、许可可商用、轮廓与栅格化均为业界成熟实现；`stb_truetype` 为单头文件 public domain 实现，适合纯 C 静态链接的客机用户态，不引入 FreeType 等重型依赖 |
| 边界 | 字体以 `objcopy -I binary` 转为目标文件**静态链入** `/sbin/aether-compositor`，不作为系统盘内独立文件，避免 ext4 注入大文件引发的 journal / 位图不一致。布局度量保持 ASCII 8px / CJK 16px 格不变，既有 UI 布局零改动；未找到字体资产时回退原点阵后端 |

**裁切根因与修复**：旧点阵把参数 `y` 当作字形第一行、直接硬画 16 行；矢量字形的位图框相对基线为负偏移（如 `y0=-10`），沿用旧逻辑会让字形整体下移并裁掉底部。现按 `py = y + asc_px + y0` 换算基线，使字形完整落入行盒。验证：host 定量断言「行盒内有墨点、行盒下方零墨点、灰度级数 > 200（证明抗锯齿生效）」全部通过；ARM64 full compositor 构建、注入 v6 盘、QEMU TCG 真机闭环（`run_v6.sh` 退出码 0、阶段 5 断言全 OK、无崩溃与 ext4 错误）通过；从真机桌面帧目视确认中文清晰完整、无底部裁切、无缺字形方框。

### 二、应用内更新器

| 项 | 内容 |
|---|---|
| 分发通道 | GitHub Releases（公开仓库 `sitong-zhang/AetherOS-Phone` 的 latest Release，固定资产名 `AetherVM.apk`），仅使用公开 REST API 与公开下载直链 |
| **采用理由** | 版本更新直接复用 GitHub 既有的 Release 能力，不再手工分发安装包。`/releases/latest` 已提供 `tag_name`、资产 `size` 与 `sha256:` digest，可直接用于版本比较与完整性校验，无需自建更新服务，也无需重复造签名与分发轮子 |
| 实现 | `vm/android/java/dev/aether/vm/` 下 `UpdateManager`（网络、版本比较、digest 校验）、`UpdateProvider`（自研 `ContentProvider`，以 `content://` 暴露私有目录中的 APK）、`UpdateActivity`（纯 Java 界面，无 androidx 依赖）；入口挂在系统诊断浮层与崩溃报告页 |
| 权限边界 | 新增 `INTERNET` 与 `REQUEST_INSTALL_PACKAGES`；Android 8+ 经 `ACTION_MANAGE_UNKNOWN_APP_SOURCES` 申请安装未知应用授权；**不内置任何访问凭证**（公开读取不需要认证），不申请外部存储读写权限，更新包落在应用私有目录 |
| 合规 | 未引入第三方更新 SDK（无 androidx / 无 Firebase / 无第三方自升级框架），全部为 Android 平台原生 API |

**验证边界**：Java 源码编译与 dex 打包校验通过；更新包下载、`sha256:` 校验与 `content://` 安装为 Android 系统能力，需在真机安装后触发验证，本仓库的 QEMU 客机闭环不覆盖宿主侧安装流程。
