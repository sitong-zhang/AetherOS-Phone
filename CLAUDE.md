# CLAUDE.md

本仓库是 **AetherOS-Phone**：基于 Linux 内核（ARM64 Linux 6.6.70）+ 完全自研用户态的
**手机操作系统，主打安全**。目标是刷入开发板与谷歌 Pixel 真机实测；当前在云电脑上做系统
构建与调试，软件模拟只是开发手段。**系统本体就是真 OS，不做 APK、不套壳、不交付预览包**
（详见 [`AGENTS.md`](AGENTS.md)）。

[`AGENTS.md`](AGENTS.md) 收录环境/工具链、各组件构建命令、测试与 CI 运行方式、已知坑、
待办清单、关键文件索引与代码约定。架构与原理文档见 [`docs/`](docs/README.md)。

文档索引：
- 构建 / 启动 / 测试 / 排查 → `AGENTS.md`
- 整体架构 → `README.md`
- 工程细节与诚实边界 → `docs/README.md`
