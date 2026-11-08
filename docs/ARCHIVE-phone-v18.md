# phone-v18 及以前：旧交付方式归档说明

`legacy/` 保存的是 AetherOS 早期「Termux + QEMU + WebView/VNC」交付方式的完整实现：

- `legacy/userland/`：多进程用户态源码（init、aetherosd、窗口管理器式 compositor、SystemUI、`.aepkg` 包管理、沙箱、logd/crashd/watchdog、A/B OTA）、内核与 initramfs 的 Git LFS 指针
- `legacy/scripts/`：Termux 安装、启动、rootfs 打包、phone-app.sh 等脚本
- `legacy/ci/`、`legacy/.github/`：旧版 CI 与 smoke 断言
- `legacy/docs/`（若存在）及 `docs/legacy/`：v18 归档、根因分析与历史安装文档
- `legacy/assets/`、`legacy/third_party/`、`legacy/tools/`、`legacy/dist-phone/`：资源与旧发布物

该实现已于 2026-10 停止迭代，最后一个 Release 为 `phone-v18`。其 `v18-notes.md` 记录的 WebSocket 桥接帧长度字段修复只适用于旧 WebView 通道，与当前主线无关。

**保留价值**：`docs/AETHEROS-OS-LINEAGE.md`（开源系统谱系与逐项借鉴清单）、图形栈审计、服务协议文档、多进程合成器与包管理签名实现，是后续章节（沙箱、`.aepkg` 签名、A/B OTA、logd）可直接取用的参考实现；`aether-font.h` 与当前主线逐字节相同。旧实现不再参与构建，也不与新工程同名文件混用。
