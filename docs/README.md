# docs/ —— 工程文档索引

本目录收录 AetherOS 主线的工程文档。仓库总览见根目录 [`README.md`](../README.md)。
当前唯一需求源为《AetherOS 需求总文件 v2》；本仓库早期的「自包含 APK 内嵌虚拟机」
交付形态已归档停用，见 [`../vm/DEPRECATED.md`](../vm/DEPRECATED.md)。

## 文档清单

| 文档 | 内容 |
| --- | --- |
| [`00-BASELINE.md`](00-BASELINE.md) | 本机实测环境基线（v2 §2.1 重填）、测量方法、与参考环境差异、变更记录 |
| [`01-STAGE-REPORT.md`](01-STAGE-REPORT.md) | 第一阶段报告：四件套（改了什么/为什么/怎么验证/未解决）、模块三档状态表 |
| [`02-SECURE-REPORT.md`](02-SECURE-REPORT.md) | 第二阶段报告：安全整机四件套、真实缺陷清单、验收证据、诚实边界 |
| [`SECURITY.md`](SECURITY.md) | **安全系统设计**：威胁模型、四区隔离、认证/保险库/自毁引擎、14 项特性、边界 |
| [`KERNEL.md`](KERNEL.md) | **自研 x86_64 内核**：架构、引导链关键坑（U/S 两级/系统调用映射/ATA 时序/摘要哈希）、系统调用、内核态安全、边界 |
| [`03-KERNEL-STUDY.md`](03-KERNEL-STUDY.md) | 第三轮：开源 OS 教程调研与取舍（先搜后学，再落地） |
| [`04-KERNEL-REPORT.md`](04-KERNEL-REPORT.md) | 第四轮：ring3 安全 demo 真机闭环 + 4 个内核 bug 四件套 + 证据清单 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 八层架构、双轨、L2 十五域接口、§5.6 五门机检、阶段与门禁、参数治理、真机路线（含自研内核形态说明） |
| [`../guest/AetherOS-系统进度说明.md`](../guest/AetherOS-系统进度说明.md) | 历史阶段 1–5 各模块落地状态（旧交付口径，仅供追溯，不作 v2 验收证据） |
| [`../guest/OPENSOURCE-ATTRIBUTION.md`](../guest/OPENSOURCE-ATTRIBUTION.md) | 图标与第三方开源资产归属、许可证与采用理由 |
| [`../LICENSE`](../LICENSE) | 仓库许可证 |

## 证据与工程入口

| 路径 | 内容 |
| --- | --- |
| `../tools/collect_baseline.py` | 环境基线采集（可复现，输出 JSON） |
| `../tools/gate_l2.py` | §5.6 五门机检，规则固定、命中即失败、不设豁免 |
| `../scripts/aether.sh` | §2.2 六种一键操作统一入口（另有 `secure` 转发分支） |
| `../scripts/secure.sh` | 安全整机一键入口：build/test/demo/evidence/status/shell/all |
| `../scripts/kernel.sh` | 自研内核一键入口：build/test/evidence/gate（QEMU TCG 真启动 + 取证 + 门禁） |
| `../tools/render_secure_ui.py` | 由真实运行态快照渲染界面图（取证，非概念图） |
| `../tools/kernel_qemu_driver.py` | 内核 QEMU 驱动：unix socket 串口 + QMP 真实 VGA 帧缓冲取证 + 结构化断言 |
| `../tools/gate_kernel.py` | 内核六门机检：构建/密码学/隔离/参数单一源/可复现/Multiboot |
| `../kernel/` | 自研 x86_64 内核：引导/分页/驱动/系统调用/ring3 init/内核态安全 |
| `../l2/` | L2 接口层：定义、仿真/桩实现、自测 |
| `../secure/` | 安全整机本体：密码学、存储、认证、保险库、自毁、审计、CLI、自测 |
| `../evidence/round-001/` | 第一轮四件套证据与第 1 门存量命中归因清单 |
| `../evidence/round-002/` | 第二轮：81 项自测、OpenSSL 交叉验证、整机演示日志、3 份快照、4 张界面图、销毁证明 |
| `../evidence/round-003/` | 第三轮内核：真实串口全程、QMP 真实 VGA 帧缓冲截图、销毁证明、34 项断言 JSON、退出码 |

## 约定

- 状态标注遵循诚实原则：未跑通一律写「部分实现 / 未开始」；读不到的值标 `unknown`，
  不编造；虚报按不合格处理（v2 §13）。
- 门禁规则、断言阈值、验收期望值不由执行方修改（v2 §11.3）；改动须留 diff 与回滚点。
- 一轮只改一类问题（v2 §2.7），改完重跑同一用例 + 冒烟集，通过才计入「已修」。
- 面向公众的文档使用客观陈述，不出现对话语气、不含访问凭证；不维护独立公网发布页。
