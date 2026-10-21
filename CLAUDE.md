# CLAUDE.md

本仓库是 **AetherOS-Phone**：ARM64 Linux 6.6.70 内核 + 完全自有用户态的移动操作系统，
手机交付形为 Android App 内嵌 QEMU 跑客机。

接手本仓库时，**先读 [`AGENTS.md`](AGENTS.md)** —— 它包含环境/工具链、每个组件的构建命令、
测试与 CI 运行方式、已知坑、待办清单、关键文件索引与代码约定。架构与原理文档在 [`docs/`](docs/README.md)。

快速定位：
- 想构建/启动/测试/排查 → `AGENTS.md`
- 想理解整体架构 → `README.md`
- 想查工程细节与诚实边界 → `docs/README.md`
