# AetherOS 真 Linux 内核 —— 子系统扩展 (Phase 2-B)

在「内核能启动 + BusyBox 最小用户空间」基础上，本轮扩展并**真机（QEMU）验证**了以下子系统。
全部基于本机交叉编译的 **Linux 6.6.70 (aarch64)**，未用任何模拟/伪装。

## 本轮新增 / 验证的子系统

| 子系统 | 内核选项 | 验证证据 |
|---|---|---|
| **virtio 块设备** | `VIRTIO_BLK=y` | `virtio_blk virtio2: [vda] ... (537 MB)`；`VFS: Mounted root (ext4 filesystem) on device 254:0` |
| **真实磁盘 rootfs** | `EXT4_FS=y` | 用 ext4 镜像（非 initramfs）作 `/` 挂载，读写模式 |
| **virtio 网络** | `VIRTIO_NET=y` + `NET_9P`/网络栈 | `device=eth0, ipaddr=10.0.2.15`，DHCP 自动获址成功 |
| **9p 共享目录** | `NET_9P=y` / `9P_FS=y` / `NET_9P_VIRTIO=y` | `9p: hostshare mounted at /mnt/share` |
| **虚拟文件系统** | `PROC_FS`/`SYSFS`/`DEVTMPFS`/`TMPFS` | proc/sys/devtmpfs 均挂载（诊断计数 3） |
| **多核 SMP** | `SMP=y` | `smp: Brought up 1 node, 2 CPUs` |
| **硬件熵源** | `HW_RANDOM_VIRTIO=y` | virtio 随机数（避免 `/dev/random` 阻塞） |
| **串口控制台** | `SERIAL_AMBA_PL011=y` | `ttyAMA0 at MMIO ... is a PL011` |

## 启动流程（真实）

1. QEMU (`-M virt`) 加载交叉编译的 `Image`（真内核）。
2. 内核识别 virtio 磁盘 `vda` 并挂载 ext4 rootfs 为 `/`。
3. `rdinit`/init → BusyBox init 读 `/etc/inittab`。
4. `rcS` 挂载 proc/sys/devtmpfs、拉起 eth0（DHCP）、挂 9p 共享到 `/mnt/share`。
5. getty 在 `ttyAMA0` 给出 `aetheros login:` 真实登录提示符。

## 文件清单

- `aetheros-kernel-6.6.70-aarch64.Image` —— 交叉编译的 aarch64 内核（含全部上述子系统）
- `aetheros-rootfs.ext4` —— 真实 ext4 根文件系统（BusyBox + 标准目录 + 账户 + inittab/rcS）
- `boot.sh` —— 一键启动（带 virtio 磁盘 / 网络 / 9p）
- `hostshare/` —— 宿主共享目录，guest 内 `/mnt/share` 可见（9p）
- `aetheros-initramfs.cpio.gz` —— 上一阶段最小 initramfs（保留，作对比/兼容）

## 复现

```bash
sudo apt-get install qemu-system-arm
cd aether-linux
./boot.sh          # Ctrl-A 然后 X 退出
```

## 诚实边界

- 这是**真内核 + 真实根文件系统 + 真实设备（virtio）**，在 QEMU 里实跑，非模拟。
- 目前用户空间是 BusyBox 最小系统；AetherOS 的 Compose UI / 系统服务尚未移植到该 Linux 用户空间（下一步 Phase A 的路线）。
- 9p 共享、virtio 网络在本机 QEMU 已验证；上真机/开发板需对应设备树与驱动（Phase 13）。
