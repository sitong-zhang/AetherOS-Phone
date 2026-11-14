# AetherOS

AetherOS 是一套**基于 Linux 内核的移动操作系统**，面向手机与开发板，以**安全**为核心设计目标。
内核采用 Linux（ARM64），用户态自研，不基于 AOSP，也不基于现成发行版。系统以完整 OS 形态交付，
不以 Android 应用、安装包或预览壳的形式提供。

项目当前在云计算机上完成系统构建与调试，使用 QEMU 软件模拟验证整机链路；硬件相关能力通过
可替换的接口层定义，在真机移植阶段接入具体驱动。目标设备为开发板与谷歌 Pixel 系列真机。

## 安全特性

AetherOS 的安全能力集中在 `secure/` 用户态安全整机中，全部能力以可编译、可运行、可验证的代码
实现，并通过自测与 OpenSSL 交叉验证：

- **密码学基座**：SHA-256、HMAC-SHA256、PBKDF2、ChaCha20、encrypt-then-MAC，自实现并与
  OpenSSL 标准向量交叉验证一致。
- **隔离存储与加密保险库**：分区隔离、全盘静态加密、会话密钥仅驻内存、锁定即清零。
- **触发式数据销毁**：口令错误达阈值时抹除用户数据区并保留系统本体；胁迫口令进入诱饵环境；
  篡改审计链或计数文件即触发销毁；销毁结果可离线校验。
- **防篡改审计链**：以设备密钥派生审计密钥，对链式记录做 HMAC，防止无密钥重算伪造历史。
- **TEE 密钥锚点**：设备主密钥由可替换的安全锚点提供，按域派生子密钥；软件后端用于当前开发环境，
  真机后端对接安全芯片与 eFuse。
- **设备自证（Attestation）**：以设备密钥对安全状态签名，输出可离线验证的完整性报告。
- **安全锁定 / 熔断模式**：首启强制设定零存储专用安全密码；组合键进入锁定态后仅该密码可解锁；
  物理与接口事件经**攻击意图判定层**分级，仅「确认攻击意图」触发软件熔断与硬件熔断指令。
  详见 `docs/SECURITY-BASELINE.md`。

## 攻击意图分级

物理信号本身只作为证据，是否触发熔断由攻击意图判定层依据结构化观测推导：

| 级别 | 观测特征 | 系统动作 |
| --- | --- | --- |
| L1 无害 | 仅充电、常规已鉴权传输、维修开盖、未触核心区的测试点短接 | 记录取证，零删除 |
| L2 可疑 | 未授权调试接口枚举、鉴权失败、存储枚举、核心区侦察读 | 断开连接、记录取证、进入可疑态，零删除 |
| L3 确认攻击 | 核心资产区导出、密钥区触碰、拆机短接取核心数据、未鉴权调试接口执行刷写 | 软件熔断 + 硬件熔断指令 + 报警 |
| SERVICE 维修 | 有效签名维修凭证 | 豁免物理开盖与短接，不豁免 L3 核心区导出 |

判定层（`secure/core/aether_intent.c`）为纯函数，以 USB 枚举类别、行为动词、访问区域、鉴权结果
与物理信号为输入，输出判据等级、归一化事件与可审计理由；信息不足时按最低等级处理。
自测包含 21 例观测样本，非攻击样本误判为攻击的数量为 0。

软件熔断（抹除系统分区、引导级拦截、刷机可重装）与硬件熔断（真机 eFuse/OTP 物理永久变砖）
分开定义与验收。当前开发环境不接物理烧毁器件，硬件熔断为仿真指令锁存。

## 架构

系统按八层组织：内核、硬件接口层、系统服务、框架、应用、视觉、兼容层、构建与验收。
硬件访问只在接口层及以下发生，上层源码通过十五域 HAL 访问设备能力，接口实现可在仿真后端与
真机驱动之间替换，上层代码保持同源。完整分层与门禁定义见 `docs/ARCHITECTURE.md`，
整体设计与阶段状态见 `docs/SYSTEM-BLUEPRINT.md`。

验证采用双轨：A 轨在 QEMU 上验证「引导 → 内核 → 挂载根文件系统 → init → 服务 → 首帧」链路；
B 轨以同一份上层源码原生运行，验证交互、像素与长稳。两轨代码同源，差异仅在构建目标与接口实现。

## 目录

| 路径 | 内容 |
| --- | --- |
| `secure/` | 用户态安全整机：密码学、隔离存储、认证、保险库、销毁引擎、审计链、TEE 锚点、设备自证、攻击意图判定层、物理安全接口、安全锁定/熔断、整机 CLI 与自测 |
| `guest/` | 自研用户态上层：init、合成器、渲染、字体、图标、桌面与系统应用、设计参数单一源 |
| `l2/` | 十五域硬件接口层：统一接口定义、仿真实现、桩实现与接口自测 |
| `boot/` | 整机引导链路：Linux 内核配置、自建根文件系统、QEMU 启动与开机门禁 |
| `scripts/` | 一键构建、启动、关闭、重建、跑测、取证入口 |
| `tools/` | 环境基线采集、静态机检、开机门禁、证据渲染 |
| `docs/` | 产品规格与架构文档 |

## 状态

| 子系统 | 状态 |
| --- | --- |
| `secure/` 用户态安全整机 | 已实现，356 项自测全部通过，OpenSSL 交叉验证 4/4 一致 |
| `l2/` 硬件接口层 | 部分实现，十五域接口与仿真实现在建 |
| `guest/` 用户态上层 | 部分实现，按接口层解耦中 |
| `boot/` 整机引导链路 | 部分实现，Linux 内核与根文件系统构建链路在建 |
| 软键盘（含中文输入） | 规划中 |
| 真机移植（开发板 / Pixel） | 规划中 |

`secure/` 当前以本机 CLI 形式运行，随 Linux 手机 OS 启动的服务化集成、验证启动、内核硬化、
真机 TEE 与物理防拆将在后续阶段接入。物理事件检测在开发环境中为结构化观测注入，真机阶段由
检测层输出同样的观测结构。

## 快速开始

```bash
# 环境基线
python3 tools/collect_baseline.py

# 六种一键操作，统一入口
scripts/aether.sh build       # 从源码构建
scripts/aether.sh start       # 启动整机链路 / 原生上层
scripts/aether.sh stop        # 关闭并清理进程、挂载与锁文件
scripts/aether.sh rebuild     # 清干净重来，产出可复现哈希
scripts/aether.sh test        # 自测 + 静态机检
scripts/aether.sh evidence    # 导出日志、状态与哈希证据

# 安全整机
scripts/secure.sh build       # 以 -Werror 编译整机与自测
scripts/secure.sh test        # 全套自测 + OpenSSL 交叉验证
scripts/secure.sh shell       # 交互式整机 shell
```

### 观测判定示例

```bash
BIN=build/bin/aether-os; R=/tmp/aether-observe; PW='Abc!Def@Ghi#Jkl$Mn0PqRs'; rm -rf "$R"
$BIN --root "$R" --no-delay --ld-now 1000  ld-provision --pw "$PW" --now 11000
$BIN --root "$R" --no-delay --ld-now 40000 ld-gesture --now 40000
$BIN --root "$R" --no-delay ld-observe usb=fastboot                     # L2 可疑，零删除
$BIN --root "$R" --no-delay ld-observe short=1 chassis=1               # L1 无害，零删除
$BIN --root "$R" --no-delay ld-observe usb=fastboot verb=export-core region=sys-core  # L3 熔断
```

## 工程约定

- 设计参数集中在 `guest/src/aether-tokens.h`，业务代码不出现裸数值。
- 硬件访问仅在接口层及以下；上层源码禁止直连设备节点与内核接口。
- 验收标准与门禁规则固定，不由实现方修改；每次改动保留状态断言与可复现证据。
- 外部资产优先随仓库提供；必须外取时给出校验和与许可声明，缺失显式报错。
- 测试密钥与访问凭证不写入仓库。

## 文档

| 文档 | 内容 |
| --- | --- |
| [`docs/SYSTEM-BLUEPRINT.md`](docs/SYSTEM-BLUEPRINT.md) | 系统蓝图：定位、整体思路、分层、双轨、状态与阶段路线 |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 八层架构、十五域接口、机检门禁与阶段定义 |
| [`docs/SECURITY-BASELINE.md`](docs/SECURITY-BASELINE.md) | 安全规格：威胁模型、三档数据保护、安全锁定/熔断、逐条验收口径 |
| [`docs/README.md`](docs/README.md) | 文档索引 |
| [`guest/OPENSOURCE-ATTRIBUTION.md`](guest/OPENSOURCE-ATTRIBUTION.md) | 图标与第三方资产归属、许可证 |

## 开源归属

图标取自 Lucide（`lucide-static v0.468.0`，ISC），离线光栅化编入源码；矢量字体为
Noto Sans CJK SC 的 OFL 修改版子集，由 `stb_truetype` 栅格化。液态玻璃相关上游依赖与逐条
对照见 `guest/OPENSOURCE-ATTRIBUTION.md` 与 `guest/THIRD_PARTY/`。

## 许可证

见 [`LICENSE`](LICENSE)。第三方资产许可见上节归属文档。
