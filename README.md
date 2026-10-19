# AetherOS-Phone

[![Release](https://img.shields.io/github/v/release/sitong-zhang/AetherOS-Phone?label=release)](https://github.com/sitong-zhang/AetherOS-Phone/releases)
[![Image size](https://img.shields.io/github/download-size/sitong-zhang/AetherOS-Phone/phone-v4/aetheros-phone.tar.gz?label=image%20size)](https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v4/aetheros-phone.tar.gz)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

在安卓手机上通过 **Termux + QEMU** 纯软件模拟运行完整 AetherOS：
真实 ARM64 Linux 6.6.70 内核 + 独立用户态，无需 root / 无需解锁 bootloader。
窗口为 **iOS 风格液态玻璃**（背景实时模糊 + 边缘折射 + 镜面高光，CPU 软件
渲染），并带 **安卓兼容层**：把真机电量/充电/Wi-Fi/通知经 HAL 后端喂给系统。

## 一条命令：自动安装并运行最新版

Termux 里执行这一行，它会**自动探测 GitHub 最新 release、下载、解包并启动**——永远跑最新版（将来出 phone-v4/v5 也照跑）：

```sh
curl -fsSL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/boot-latest.sh | sh
```

- 自动探测最新版本 → 下载镜像（约 20MB）→ 解压 → 前台启动
- 启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`
- 交互：数字键 **1-9** 或 **手指点击 Launcher 应用行** = 启动对应应用
- 退出：Ctrl-A 再按 X；低内存手机：`AETHER_MEM=384M AETHER_SMP=1` 前缀同一行

## 一行安装（手机 Termux）

```sh
curl -sL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/install-termux.sh | sh
```

脚本会自动：装依赖 → 下载镜像（约 20MB）→ 解压 → 后台启动 → 打印连接信息。
启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`，VNC 窗口内按数字键 **1** 启动 demo 应用。

> 只装不启动：`sh install-termux.sh --no-run`

## 手动安装

```sh
pkg install qemu-system-aarch64-headless   # 主仓库正确包名（二进制仍叫 qemu-system-aarch64）
mkdir -p ~/aetheros && cd ~/aetheros
curl -L -o aetheros-phone.tar.gz https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v4/aetheros-phone.tar.gz
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

镜像详见 Releases 页的 `phone-v4`。

**phone-v4**（2026-10-19 · 看门狗误杀修复）：修复慢速手机（纯软件模拟冷启动慢）上
合成器被看门狗反复误判僵死、处决重启的死循环——新进程启动宽限期（首次成功心跳前
不计 miss，120s 封顶，内核命令行 `aether_wdt_grace=` 可调）、心跳超时 2s→5s、
合成器提前绑定 IPC socket。CI 92 项启动断言全绿（含触摸端到端 T2）。

## 诚实边界

- 模拟运行，TCG 冷启动约 1-2 分钟，操作有可感延迟（不解锁 bootloader 的唯一代价）
- `-snapshot` 模式：退出后客机写入全丢，镜像玩不坏
- 通话 / 移动上网不可用（无 RIL 基带层）
