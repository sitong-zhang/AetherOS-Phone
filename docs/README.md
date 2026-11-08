# docs/ —— 工程文档索引

本目录收录 AetherOS-Phone 主线的工程文档。仓库总览见根目录 [`README.md`](../README.md)，
构建与操作说明见 [`AGENTS.md`](../AGENTS.md)。

## 文档清单

| 文档 | 内容 |
| --- | --- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 自包含 APK 内嵌虚拟机的分层结构、启动链、与早期交付方式的差异、真机验证链 |
| [`../guest/AetherOS-系统进度说明.md`](../guest/AetherOS-系统进度说明.md) | 阶段 1–5 各模块的落地状态与诚实边界 |
| [`../guest/OPENSOURCE-ATTRIBUTION.md`](../guest/OPENSOURCE-ATTRIBUTION.md) | 图标与第三方开源资产归属、许可证与采用理由 |
| [`../LICENSE`](../LICENSE) | 仓库许可证 |

## 约定

- 状态标注遵循诚实原则：读不到的值标 `unknown` / `-1` / `false`，不编造；断言期望值取自真实取证日志。
- 行为变更后同步更新对应文档的状态与边界说明。
- 面向公众的文档与发布说明使用客观陈述，不出现对话语气。
