# AGENTS.md —— 接手者操作手册（主线：自包含 APK）

## 0. 一句话定位

AetherOS = ARM64 Linux 6.6.70 内核 + 完全自研用户态。主线交付为**自包含 Android APK 内嵌 QEMU 虚拟机**，点按一次开机进桌面。早期 Termux / WebView / VNC 交付方式已停止维护，相关源码与文档已从本仓库移除。

## 1. 目录

| 路径 | 说明 |
| --- | --- |
| `guest/src/` | 客机 C 源码（合成器、init、渲染、字体、图标、阶段 1–5 业务层） |
| `guest/build-aarch64.sh` | 交叉编译静态 aarch64 ELF，`AETHER_BUILD` 可重定向输出目录 |
| `vm/android/` | APK 的 Java/JNI 层、res、`AndroidManifest.xml` |
| `vm/core/`, `vm/host/` | VM 运行时、宿主闭环程序、`vm.conf`、自检脚本 |
| `vm/make_system_v6.sh` | 生成 ext4 qcow2 系统盘（debugfs 注入 + e2fsck 修复 + SHA-256 校验） |
| `vm/build_apk.sh` | 打包并签名 APK，内置 kernel.img / system.qcow2 / vm.conf |
| `vm/host/run_v6.sh` | 全系统 QEMU TCG 真机级闭环验证（帧取证 + 状态断言） |

## 2. 常用命令

```sh
# 构建客机用户态（需 aarch64-linux-gnu-gcc）
AETHER_BUILD=<输出目录> bash guest/build-aarch64.sh

# 生成干净系统盘（自动处理 ext4 journal）
bash vm/make_system_v6.sh

# 全系统真机闭环验证
bash vm/host/run_v6.sh

# 打包 APK（需 Android SDK/NDK、SDL2 与 QEMU 动态库闭包）
bash vm/build_apk.sh
```

## 3. 已知坑

- **ext4 journal**：基线盘 superblock 带 `needs_recovery` 时，`debugfs` 注入的大文件会在客机挂载或 e2fsck 回放 journal 时被回滚，表现为 compositor `SIGSEGV` / `Structure needs cleaning`。注入前必须 `e2fsck -fy` 清空 journal，注入后 `e2fsck -fn` 复检，并对关键文件做 SHA-256 内容校验。
- **不要重复 mkdir**：对已存在的 `/data` / `/data/aether` 再执行 `debugfs mkdir` 会产生 orphan directory inode，导致位图不一致。
- **日志通道**：客机 compositor 走 fd=2，由 PID 1 重定向到 `/dev/console`，宿主侧从串口日志读取；不要用 stdio 缓冲输出做崩溃诊断。
- **资产路径**：`vm/build_apk.sh` 与 `make_system_v6.sh` 当前引用本机预置资产绝对路径，换机构建需先准备内核 Image、系统盘与动态库闭包。
- **fbdev 后端**：客机显示走 `/dev/fb0`（`AETHER_BACKEND=fbdev`），与 PC 侧 DRM/KMS 路径不同，不要混用构建宏。

## 4. 提交约定

- 未通过真机闭环验证的功能不得标记为完成；状态文档如实标注「部分实现 / 未开始」。
- 凭证、密钥、token 不得写入仓库内任何文件。
- 交付渠道只有本 GitHub 仓库（源码与 GitHub Release），不维护独立公网发布页。版本说明与仓库文档面向公众，使用客观陈述，不出现对话语气。
- 每次二进制或系统盘变化，需重新校验 APK 哈希、版本与发布信息一致性。
