# AetherOS Phone

AetherOS 是一个从零构建的 ARM64 Linux 手机操作系统：**Linux 6.6.70 内核 + 完全自研用户态**，不含任何 AOSP 组件，不运行 ART；应用为静态链接的原生 ELF。

本仓库当前主线的交付形态是**自包含 Android APK**：APK 内置 QEMU 虚拟化引擎、客机内核镜像与系统盘（qcow2）。安装后点按一次开机即直接进入系统桌面，无需 root、无需解锁 bootloader、无需 Termux、无需 VNC 查看器、无需额外下载系统镜像。

> 早期交付方式采用「Termux 中运行 QEMU + WebView/VNC 查看画面」，其源码与文档已停止维护并从本仓库移除。当前仓库仅包含自包含 APK 主线的源码与文档。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `guest/` | 客机用户态源码：自研 init、软件合成器、渲染/字体/图标、阶段 1–5 业务层（桌面、灵动岛、应用商店、笔记、多任务、小窗、息屏显示），以及交叉编译与取证脚本 |
| `vm/` | 自包含 APK 工程：Android Java/JNI 层、VM 运行时（QEMU 参数组装）、系统盘注入脚本、APK 打包脚本、真机闭环验证脚本 |
| `docs/` | 架构说明、工程文档索引、开源归属与阶段进度文档 |

## 启动链

`BootActivity` → VM 运行时组装 QEMU 参数（`vm/core/`）→ 客机 Linux 6.6.70 内核 → `/sbin/aether-init` → `/etc/aether/run-compositor` → `/sbin/aether-compositor`（fbdev `/dev/fb0` 软件渲染）→ 锁屏 / 桌面。

客机系统盘为 ext4 qcow2，由 `vm/make_system_v6.sh` 以 `debugfs` 注入生成。注入前必须先执行 `e2fsck -fy` 回放并清空 ext4 journal，注入后再做一致性复检；否则残留 journal 会在客机挂载时回滚大文件数据块，导致 exec 阶段崩溃。该问题曾导致 v6 真机启动失败，现已修复并在脚本中加入断言。

## 当前状态（如实标注）

- 版本 `1.0.16` / `versionCode 15`，包名 `dev.aether.vm`。
- 主线已按《Aether OS 总规划书》推进至**阶段 5**：应用商店与安装/卸载闭环、笔记应用、多任务与最近任务、小窗、息屏显示（AOD）。阶段 6 完成全量复跑取证、系统盘注入与 APK 重打包。
- 阶段 5 断言链已在 v6 系统盘上通过全系统 QEMU TCG 真机级闭环复跑：逐帧取证、`SELFTEST_STAGE5_DONE=1`、compositor 全程在线（fbdev 720×1280）、无崩溃与文件系统错误。验证入口 `vm/host/run_v6.sh`。
- **矢量字体**：客机文字已从 16×16 点阵升级为内嵌 TrueType 矢量字体（`guest/assets/aether-font.ttf`，Noto Sans CJK SC 的 OFL 修改版子集，7720 码点），由 `stb_truetype` 做抗锯齿灰度栅格化。保持 8/16px 布局格不变，按基线换算修复了文字底部裁切；字体以 `objcopy` 静态链入合成器，不作为系统盘内的独立文件。许可证与采用理由见 [`guest/OPENSOURCE-ATTRIBUTION.md`](guest/OPENSOURCE-ATTRIBUTION.md)。
- **应用内更新**：APK 内置「系统更新」界面，读取公开 GitHub Releases latest 接口获取版本与资产摘要，下载后按 `sha256:` digest 校验，再经自研 `ContentProvider` 以 `content://` 交给系统安装器。入口位于系统诊断浮层与崩溃报告页。该模块只访问公开接口，不内置任何访问凭证。
- **构建边界**：APK 打包依赖本机预置的外部资产（内核 Image、系统盘、SDL2/QEMU 动态库闭包、Android SDK/NDK），这些大二进制不纳入本仓库；仓库内暂未提供一键可复现的完整构建。后续工作项之一是把资产生成脚本化并接入 CI。
- **交付阶段**：当前为内部调试阶段，系统尚未对外发布。版本通过 GitHub Release 分发，供应用内更新器读取；本仓库不提供面向公众的宣传页面。
- 尚未完成：总纲后续章节、真机下载安装与升级链路验证。

## 开源归属

- 图标与第三方资产归属、许可证与采用理由：[`guest/OPENSOURCE-ATTRIBUTION.md`](guest/OPENSOURCE-ATTRIBUTION.md)
- 图标统一取自 Lucide（`lucide-static v0.468.0`，ISC），由 `guest/tools/gen_icons.py` 离线光栅化编入源码。

## 许可证

见 [`LICENSE`](LICENSE)。
