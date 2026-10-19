# AetherOS-Phone

[![Release](https://img.shields.io/github/v/release/sitong-zhang/AetherOS-Phone?label=release)](https://github.com/sitong-zhang/AetherOS-Phone/releases)
[![Image size](https://img.shields.io/github/download-size/sitong-zhang/AetherOS-Phone/phone-v1/aetheros-phone.tar.gz?label=image%20size)](https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v1/aetheros-phone.tar.gz)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

在安卓手机上通过 **Termux + QEMU** 纯软件模拟运行完整 AetherOS：
真实 ARM64 Linux 6.6.70 内核 + 独立用户态，无需 root / 无需解锁 bootloader。

## 一行安装（手机 Termux）

```sh
curl -sL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/install-termux.sh | sh
```

脚本会自动：装依赖 → 下载镜像（约 20MB）→ 解压 → 后台启动 → 打印连接信息。
启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`，VNC 窗口内按数字键 **1** 启动 demo 应用。

> 只装不启动：`sh install-termux.sh --no-run`

## 手动安装

```sh
pkg install qemu-system-aarch64
mkdir -p ~/aetheros && cd ~/aetheros
curl -L -o aetheros-phone.tar.gz https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v1/aetheros-phone.tar.gz
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
| `phone-boot.sh` | 启动脚本（内存/CPU 调优） |
| `README-phone.md` | 完整使用说明 |

## 发布说明

镜像详见 Releases 页的 `phone-v1`。

## 诚实边界

- 模拟运行，TCG 冷启动约 1-2 分钟，操作有可感延迟（不解锁 bootloader 的唯一代价）
- `-snapshot` 模式：退出后客机写入全丢，镜像玩不坏
- 通话 / 移动上网不可用（无 RIL 基带层）
