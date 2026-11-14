# AetherOS

AetherOS 是一套面向移动设备的操作系统。系统落地层是**自研 x86_64 内核**（非 Linux 复用，
从零实现引导 / 4 级分页 / 驱动 / 系统调用 / ring3 用户态），用户态完全自研；不基于 AOSP、
不基于现成发行版。

> 内核形态说明（如实）：v2 总文件的产品目标是「Linux 内核 + 自研用户态」；按用户直接指令，
> 「要有真正的内核、去开源仓库学别人怎么做操作系统」，我们落地的是**独立自研 x86_64 内核**
> （`kernel/`，见 [`docs/KERNEL.md`](docs/KERNEL.md)），并已在本机 QEMU TCG 真实启动跑通
> ring3 安全 demo 全闭环（34/34）。Linux 内核方案保留为移动端量产目标，与自研内核并行，
> 不互相替代。

本项目的运行环境是**云电脑**，用软件模拟把整机真实跑起来：真实开机、真实出图、真实响应
输入。系统本体**不是 Android 应用、不是 APK、不是虚拟机套壳预览包**。

系统**主打安全**。当前已跑通两套正交的安全子系统：
- **内核态安全子系统**（`kernel/secure/`，随自研内核启动）：ring3 用户态经 `int 0x80` 完成
  认证 / 加密保险库 / 自毁；输错口令达阈值 → 销毁 DEK + 多轮覆写整卷 + 清凭据，而内核
  `.text/.rodata` 摘要硬断言前后一致（系统保留）。
- **用户态安全整机本体**（`secure/`，独立编译运行）：口令连续错误达阈值即 **抹除全部用户数据**
  （销毁密钥 + 多轮覆写 + 清除凭据），而**系统本体逐字节保留**；另有胁迫口令、防重启绕过计数、
  全盘静态加密、防篡改审计链、篡改即擦、紧急销毁等特性。
设计与验收见 [`docs/SECURITY.md`](docs/SECURITY.md)；内核侧见 [`docs/04-KERNEL-REPORT.md`](docs/04-KERNEL-REPORT.md)。

## 交付形态与双轨

一套上层源码，两条验证轨道，证据分开记录、互不顶替（见 `docs/ARCHITECTURE.md`）：

| 轨道 | 载体 | 只证明 | 不证明 |
| --- | --- | --- | --- |
| **A 轨 · 真内核链路** | QEMU x86_64 + TCG 纯软件模拟，**自研内核**（引导→分页→驱动→系统调用→ring3 init→安全服务） | 真实开机引导、真实 4 级分页/ring3 切换、真实驱动(ATA/键盘)、真实系统调用、ring3 安全 demo 闭环、真实 VGA 帧缓冲取证 | 帧率、动效手感、长稳、图形桌面、真机 TEE |
| **B 轨 · 交互与像素**（设计目标，待重做取证） | 同一份上层源码 x86_64 原生构建，接口层接仿真实现 | 交互延迟、动画帧率、布局、对比度/遮挡/裁切、状态机、长稳 | 内核启动、真实驱动 |

> A 轨早期设计口径为「ARM64 + 自建 Linux 内核」；实际落地并已真机闭环验证的是 **x86_64 自研内核**
> （`kernel/`，34/34 PASS）。ARM64 / Linux 量产方案保留为移动端目标，不计入本轮已验证项。
> 云电脑无 `/dev/kvm`，整机只能用 TCG 软件模拟，A 轨交互迟钝属环境限制。

**兼容层**：AetherOS 主系统完全自研；对既有应用格式的兼容能力由系统内一个独立子系统提供，
按容器化方式运行（可基于 AOSP / LineageOS / Waydroid 等开源实现）。兼容层内部运行的是
第三方应用，这与「系统本体是 APK」是两件不同的事。

## 目录结构

| 路径 | 内容 | 状态 |
| --- | --- | --- |
| `kernel/` | **自研 x86_64 内核**：Multiboot1 引导、IA-32e 长模式、4 级分页(含 ring3 U/S)、驱动(ATA/PS2/PIT/CSPRNG)、系统调用(int 0x80)、ring3 init、内核态安全子系统 | **已真机闭环**（34/34 PASS，六门机检 8/8） |
| `guest/` | 复用的自研用户态上层：init、合成器、渲染/字体/图标、桌面与系统应用、参数源 `aether-tokens.h` | 可复用，正按接口层解耦 |
| `secure/` | **用户态安全整机本体**：密码学基座、四区隔离存储、认证引擎、加密保险库、自毁引擎、审计链、完整性、整机 CLI 与全套自测 | 已跑通（81/0 自测） |
| `l2/` | **L2 硬件接口层**：十五域统一接口、仿真实现、接口自测 | 本阶段在建 |
| `tools/` | 基线采集、机检门禁、检测器、内核 QEMU 驱动/门禁 | 本阶段在建 |
| `scripts/` | 多种一键操作（构建/启动/关闭/重建/跑测/取证、内核、安全整机） | 本阶段在建 |
| `docs/` | 环境基线、架构、内核、阶段报告 | 本阶段在建 |
| `evidence/` | 每轮演练的四件套证据 | 累积中 |
| `vm/` | 历史 APK 套壳工程 | **已归档停用**，不再是交付形态，见 `vm/DEPRECATED.md` |

## 当前状态（三档如实标注）

> 阶段定义见 `docs/ARCHITECTURE.md`；状态只能由证据提升，未跑通一律写「部分实现 / 未开始」。

| 阶段 | 目标 | 状态 |
| --- | --- | --- |
| **K0 自研内核** | 真内核引导→分页→驱动→系统调用→ring3→安全 demo 闭环 | **已实现（带边界）**：`kernel/` 在 QEMU TCG 真启动，37/37 断言全绿、六门机检 8/8、真实 VGA 帧缓冲取证；已含 **PIT 时钟驱动的抢占式多任务**（init 安全 demo + task2 心跳双 ring3 任务真实轮转）；边界见 `docs/KERNEL.md` |
| S1 环境打通 | 一键起停重建、A 轨到首帧 | **部分实现**：基线已采集重填；移动端根文件系统/图形首帧尚未取证 |
| S2 接口层成型 | 十五域抽象 + 仿真实现 + 五门机检全过 | **部分实现**：接口层与自测在建 |
| S3 整机闭环 | 引导到桌面全通 + 软键盘（含中文）可用 | **未开始**（软键盘为一票否决硬门） |
| S4 能力补齐 | 全量模块清单无缺项、逐模块过像素断言 | **未开始** |
| S5 稳定与性能 | 长稳、六类异常自动发现、性能可对比 | **未开始** |
| S6 真机移植 | 换实现层上真机 / 树莓派 | **未开始** |

**两套安全子系统均已跑通：**
- **内核态**（`kernel/secure/`）：随自研内核启动，ring3 用户态经 `int 0x80` 完成认证/保险库/
  自毁，演示「输错 3 次 → 抹除全部用户数据 → 系统完好」并生成可离线校验的销毁证明。
  证据见 `evidence/round-003/`，报告见 `docs/04-KERNEL-REPORT.md`。
- **用户态整机本体**（`secure/`）：81 项自测全绿、OpenSSL 密码学交叉验证 4/4 一致，
  完整演示「输错 3 次 → 抹除全部用户数据 → 系统完好可再用」并生成可离线校验的销毁证明。
  证据见 `evidence/round-002/`，报告见 `docs/02-SECURE-REPORT.md`。

两套安全子系统与 S1–S6 分阶段验收正交，**不替代**图形桌面/软键盘等未开始项；纯软件模拟、
无 GUI 桌面、无真机 TEE、安卓兼容层未接入等边界如实保留。

已有的自研上层（`guest/`）此前在 QEMU TCG 上达成过连续帧变化与自检通过，但那属于
**旧 APK 套壳口径**下的证据，不作为 v2 双轨验收证据，需按新口径重新取证。

## 快速开始

```bash
# 采集环境基线（v2 §2.1 要求动手前先跑一次）
python3 tools/collect_baseline.py

# 六种一键，统一入口（详见 docs/ARCHITECTURE.md 与 scripts/）
scripts/aether.sh build       # 从源码构建
scripts/aether.sh start       # 拉起整机（A 轨）/ 原生上层（B 轨）
scripts/aether.sh stop        # 正常关闭，清理进程/挂载/锁文件
scripts/aether.sh rebuild     # 清干净重来
scripts/aether.sh test        # 全套自测 + 五门机检，输出通过/失败清单
scripts/aether.sh evidence    # 截帧、导出状态与日志

# 安全整机：一键编译 + 81 项自测 + OpenSSL 交叉验证 + 整机演示 + 导出证据（含界面图）
scripts/secure.sh all
scripts/secure.sh shell       # 进入交互式整机 shell（init/unlock/put/panic/audit...）

# 自研内核：编译 + QEMU TCG 真启动 + ring3 安全 demo 闭环 + 真实帧缓冲取证 + 断言
scripts/kernel.sh build       # 编译内核（freestanding -Werror）+ ring3 init + initrd + 虚拟盘
scripts/kernel.sh test        # QEMU TCG 真启动，自动交互测试 + 真实帧缓冲取证 + 34 项断言
scripts/kernel.sh gate        # 内核门禁六门机检（构建/密码学/隔离/参数单一源/可复现/Multiboot）
```

不需要真实手机、真实外设或某家云厂商的私有接口；换一台云电脑也要能跑。

## 工程约定

- **参数单一源**：所有尺寸、间距、时长、圆角、阈值集中在 `guest/src/aether-tokens.h`；
  业务代码不许出现裸数值，由机检脚本扫描。
- **硬件访问只在 L2 及以下**：L3 以上源码不许出现 `/dev/`、`/sys/class/`、`/proc/` 直连，
  由机检脚本判定。
- **不自改门禁**：验收标准、门禁脚本、断言阈值不由执行方修改；每次改动留 diff 与回滚点。
- **外部资产**：素材自带优先；必须外取的提供校验和与许可声明，缺失时显式报错而非静默降级。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [`docs/00-BASELINE.md`](docs/00-BASELINE.md) | 本机实测环境基线、测量方法、与参考环境差异 |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 八层架构、双轨、十五域接口、机检门禁、阶段定义 |
| [`docs/SECURITY.md`](docs/SECURITY.md) | **安全系统设计**：威胁模型、四区隔离、自毁引擎、14 项特性、边界 |
| [`docs/KERNEL.md`](docs/KERNEL.md) | **自研 x86_64 内核**：架构、引导链坑、系统调用、内核态安全、边界 |
| [`docs/03-KERNEL-STUDY.md`](docs/03-KERNEL-STUDY.md) | 第三轮内核学习：开源 OS 教程调研与取舍 |
| [`docs/04-KERNEL-REPORT.md`](docs/04-KERNEL-REPORT.md) | 第四轮内核报告：ring3 安全 demo 真机闭环 + 4 个 bug 四件套 |
| [`docs/01-STAGE-REPORT.md`](docs/01-STAGE-REPORT.md) | 第一轮 S1/S2 地基阶段报告 |
| [`docs/02-SECURE-REPORT.md`](docs/02-SECURE-REPORT.md) | 第二轮安全整机阶段报告（四件套 + 缺陷清单） |
| [`docs/README.md`](docs/README.md) | 文档索引 |
| [`guest/OPENSOURCE-ATTRIBUTION.md`](guest/OPENSOURCE-ATTRIBUTION.md) | 图标与第三方资产归属、许可证、采用理由 |

## 开源归属

- 图标统一取自 Lucide（`lucide-static v0.468.0`，ISC），离线光栅化编入源码，一套描边体系。
- 矢量字体为 Noto Sans CJK SC 的 OFL 修改版子集，由 `stb_truetype` 栅格化。
- 液态玻璃相关上游依赖与逐条对照见 `guest/OPENSOURCE-ATTRIBUTION.md` 与 `guest/THIRD_PARTY/`。

## 许可证

见 [`LICENSE`](LICENSE)。第三方资产许可见上节归属文档。
