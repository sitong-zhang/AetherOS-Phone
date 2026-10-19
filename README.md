# AetherOS-Phone

[![Release](https://img.shields.io/github/v/release/sitong-zhang/AetherOS-Phone?label=release)](https://github.com/sitong-zhang/AetherOS-Phone/releases)
[![Image size](https://img.shields.io/github/download-size/sitong-zhang/AetherOS-Phone/phone-v5/aetheros-phone.tar.gz?label=image%20size)](https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v5/aetheros-phone.tar.gz)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

在安卓手机上通过 **Termux + QEMU** 纯软件模拟运行完整 AetherOS：
真实 ARM64 Linux 6.6.70 内核 + 独立用户态，无需 root / 无需解锁 bootloader。
窗口为 **iOS 风格液态玻璃**（背景实时模糊 + 边缘折射 + 镜面高光，CPU 软件
渲染），并带 **安卓兼容层**：把真机电量/充电/Wi-Fi/通知经 HAL 后端喂给系统。

## 一条命令：自动安装并运行最新版

Termux 里执行这一行，它会**自动探测 GitHub 最新 release、下载、解包并启动**——永远跑最新版（将来出 phone-v5/v6 也照跑）：

```sh
curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh | sh
```

> 用 **github.com 域名**而不是 raw.githubusercontent.com——后者在国内网络
> 常被连接重置（`curl: (35) Recv failure: Connection reset by peer`）。
> curl 坏时用 wget 版（同一 URL）：`wget -qO- <同一URL> | sh`

- 自动探测最新版本 → 下载镜像（约 20MB）→ 解压 → 前台启动
- 启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`
- 交互：**先点按屏幕任意处解锁**（开机是锁屏），再**点按启动器应用行**或数字键
  **1-9** 启动对应应用
- 退出：Ctrl-A 再按 X；低内存手机：`AETHER_MEM=384M AETHER_SMP=1` 前缀同一行

## 一行安装（手机 Termux）

```sh
curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/install-termux.sh | sh
```

脚本会自动：装依赖 → 下载镜像（约 20MB）→ 解压 → 后台启动 → 打印连接信息。
启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`，VNC 窗口内**先点按屏幕解锁**，
再点按应用行（或数字键 **1**）启动应用。

> 只装不启动：`sh install-termux.sh --no-run`

## 手动安装

```sh
pkg install qemu-system-aarch64-headless   # 主仓库正确包名（二进制仍叫 qemu-system-aarch64）
mkdir -p ~/aetheros && cd ~/aetheros
curl -L -o aetheros-phone.tar.gz https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v5/aetheros-phone.tar.gz
tar xzf aetheros-phone.tar.gz
./phone-boot.sh
```

低内存手机：`AETHER_MEM=384M AETHER_SMP=1 ./phone-boot.sh`

## 这是什么

AetherOS 是一个从零构建的移动操作系统（ARM64 Linux 内核 + 独立用户态：init、
服务管理器、合成器、SystemUI、沙箱、日志/崩溃/看门狗可观测性栈）。本仓库提供
可在手机上直接运行的镜像包。

## 镜像内容

| 文件 | 说明 |
|---|---|
| `aetheros-kernel-aarch64.Image` | ARM64 Linux 6.6.70 内核（~41MB） |
| `aetheros-rootfs.ext4` | 根文件系统（实际占用 22MB） |
| `phone-boot.sh` | 启动脚本（内存/CPU 调优 + 9p 共享） |
| `aether-android-bridge.sh` | 安卓宿主 HAL 桥接（真机电量/网络 -> 客户机） |
| `README-phone.md` | 完整使用说明 |

## 发布说明

镜像详见 Releases 页的 `phone-v5`。

**phone-v5**（2026-10-20 · 锁屏开机 + 真应用 + 液态玻璃底层化）：

- **开机即锁屏**：全屏系统层液态玻璃面板（大时钟 + 中文日期 + 电量 + 锁图标），
  点按任意处解锁进启动器——与真手机开机语义一致
- **液态玻璃参数原样沉到底层**：直接采用 Kyant0/AndroidLiquidGlass（Apple
  Backdrop）已调好的配方——`vibrancy() + blur(4dp) + lens(16dp,32dp)`（普通窗口）/
  `lens(24dp,48dp, depthEffect)`（系统面板），不重新调参；色差/高光/内阴影全套，
  色差仅交互时开启。宿主视觉验证：玻璃透出、中文点阵真实上屏
- **真应用**：`设置`（内核/主机名/uptime/内存/负载/CPU/服务数/电量/根卷 10 行
  真实读数）与 `时钟`（大时钟 + 中文日期 + 真实秒进度条）随镜像打包，开机签名
  安装进启动器（连同 `demo` 共 4 个应用）
- **看门狗冻结盲区修复**：init 重启服务后数秒内即被冻结（SIGSTOP）时，旧逻辑会
  白放整个 120s 启动宽限；现按 /proc 调度状态识别「冻结≠慢启动」，照常处决重启
- CI **92 项启动断言全绿**（含触摸端到端 T2、锁屏/启动器/U 段 SystemUI 全链）

**phone-v4**（2026-10-19 · 看门狗误杀修复）：修复慢速手机（纯软件模拟冷启动慢）上
合成器被看门狗反复误判僵死、处决重启的死循环——新进程启动宽限期（首次成功心跳前
不计 miss，120s 封顶，内核命令行 `aether_wdt_grace=` 可调）、心跳超时 2s→5s、
合成器提前绑定 IPC socket。

## 诚实边界

- 模拟运行，TCG 冷启动约 1-2 分钟，操作有可感延迟（不解锁 bootloader 的唯一代价）
- `-snapshot` 模式：退出后客机写入全丢，镜像玩不坏
- 通话 / 移动上网不可用（无 RIL 基带层）
