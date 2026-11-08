# AetherOS Phone

AetherOS 是一个从零构建的 ARM64 Linux 手机操作系统：**Linux 6.6.70 内核 + 完全自研用户态**，不含任何 AOSP 组件，不运行 ART；应用为静态链接的原生 ELF。

本仓库当前主线的交付形态是**自包含 Android APK**：APK 内置 QEMU 虚拟化引擎、客机内核镜像与系统盘（qcow2）。安装后点按一次开机即直接进入系统桌面，无需 root、无需解锁 bootloader、无需 Termux、无需 VNC 查看器、无需额外下载系统镜像。

> 早期版本采用「Termux 中运行 QEMU + WebView/VNC 查看画面」的交付方式。该实现的完整源码与文档已归档至 [`legacy/`](legacy/)，不再维护。两种方式对应同一个操作系统，差别仅在宿主侧如何承载虚拟机与显示。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `guest/` | 客机用户态源码：自研 init、软件合成器、渲染/字体/图标、阶段 1–5 业务层（桌面、灵动岛、应用商店、笔记、多任务、小窗、息屏显示），以及交叉编译与取证脚本 |
| `vm/` | 自包含 APK 工程：Android Java/JNI 层、VM 运行时（QEMU 参数组装）、系统盘注入脚本、APK 打包脚本、真机闭环验证脚本 |
| `docs/` | 架构说明、开源系统谱系与借鉴清单、图形栈审计、服务协议；`docs/legacy/` 为旧版归档文档 |
| `legacy/` | 旧交付方式（Termux / WebView / VNC，phone-v18 及以前）的完整源码与资源，只读归档 |

## 启动链

`BootActivity` → VM 运行时组装 QEMU 参数（`vm/core/`）→ 客机 Linux 6.6.70 内核 → `/sbin/aether-init` → `/etc/aether/run-compositor` → `/sbin/aether-compositor`（fbdev `/dev/fb0` 软件渲染）→ 锁屏 / 桌面。

客机系统盘为 ext4 qcow2，由 `vm/make_system_v6.sh` 以 `debugfs` 注入生成。注入前必须先执行 `e2fsck -fy` 回放并清空 ext4 journal，注入后再做一致性复检；否则残留 journal 会在客机挂载时回滚大文件数据块，导致 exec 阶段崩溃。该问题曾导致 v6 真机启动失败，现已修复并在脚本中加入断言。

## 当前状态（如实标注）

- 版本 `1.0.15` / `versionCode 14`，包名 `dev.aether.vm`。
- 主线已按《Aether OS 总规划书》推进至**阶段 5**：应用商店与安装/卸载闭环、笔记应用、多任务与最近任务、小窗、息屏显示（AOD）。
- 阶段 5 已通过全系统 QEMU TCG 真机级闭环验证：41 帧取证、`SELFTEST_STAGE5_DONE=1`、compositor 全程在线、无崩溃与文件系统错误。验证入口 `vm/host/run_v6.sh`。
- **构建边界**：APK 打包依赖本机预置的外部资产（内核 Image、系统盘、SDL2/QEMU 动态库闭包、Android SDK/NDK），这些大二进制不纳入本仓库；仓库内暂未提供一键可复现的完整构建。后续工作项之一是把资产生成脚本化并接入 CI。
- 尚未完成：矢量字体系统（当前为 16×16 点阵 CJK）、总纲后续章节、真机下载安装验证。

## 开源归属与谱系

- 系统谱系与逐项借鉴清单：[`docs/AETHEROS-OS-LINEAGE.md`](docs/AETHEROS-OS-LINEAGE.md)
- 图标与第三方归属：[`guest/OPENSOURCE-ATTRIBUTION.md`](guest/OPENSOURCE-ATTRIBUTION.md)
- 图标统一取自 Lucide（`lucide-static v0.468.0`，ISC），由 `guest/tools/gen_icons.py` 离线光栅化编入源码。

## 许可证

见 [`LICENSE`](LICENSE)。
