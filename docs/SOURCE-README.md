# AetherOS 兼容层 —— 源码包与调试上下文

本包是 AetherOS 兼容层（自编译 Linux 6.6.70 aarch64 + Android 13 / Waydroid 用户空间）
的**源码与注入脚本**，**不含** GB 级镜像/内核二进制（`.img` / `*.cpio.gz` / `*.Image`）。
这些二进制由源码构建或直接下载得到，调试当前问题不需要它们。

## 目录结构

```
aether-linux/
├── boot.sh                      # 宿主机侧引导入口(启动 Linux 内核 + rootfs)
├── make_rootfs.sh               # 构建宿主 rootfs(ext4)的脚本(含 aether-init 与服务清单)
├── aether-init.c                # AetherOS 原生 PID 1 源码(C, 静态交叉编译 aarch64)
├── aetherosd.c                  # AetherOS 守护进程源码(C)
├── README-extension.md          # 扩展模块说明
├── README-userland.md           # 用户空间说明(aether-init / aetherosd / 监管取证 / P0-6 图形栈)
├── GRAPHICS-STACK-AUDIT.md      # P0-6 图形栈工程审计(ioctl 契约 / 已修缺陷 / 验证证据 / 诚实边界)
├── boot-evidence.log            # 历史启动证据日志(busybox init 时代)
├── aether-init-boot-evidence.log   # aether-init 首启取证(PID1/依赖序/IPC/自恢复/优雅关机)
├── aether-init-flap-evidence.log   # 防抖监管取证(连续崩溃 5 次 -> 放弃重启)
├── android/
│   ├── boot-android.sh          # ★ 关键: 启动 QEMU 跑 Android 用户空间
│   ├── acer-android-init.sh     # ★ 关键: initramfs 内引导器(挂分区/switch_root)
│   └── build-initramfs.sh       # 重建 initramfs.cpio.gz
└── hostshare/
    ├── README.txt               # 9p 共享目录说明(guest 内 /mnt/host 或 /mnt/share)
    └── inject/                  # ★ 关键: 注入到 system.img 的文件
        ├── system/bin/
        │   ├── aether-start-weston.sh    # 启动 Weston(musl chroot)并提供 wayland-0 socket
        │   └── aether-composer-probe.sh  # 诊断探针: 记录 composer 视角后 exec 回真二进制
        └── system/etc/init/
            ├── init.waydroid.rc          # ★ 关键: composer HAL 的 service 定义(已加 setenv/override)
            ├── aether-weston.rc          # Weston 服务定义
            ├── aether-logcat.rc          # logcat 落盘定义
            ├── aether-binderfs.rc        # binderfs 挂载
            ├── aether-linkerconfig.rc    # linker 配置
            ├── netd.rc                   # 网络
            └── surfaceflinger.rc         # SurfaceFlinger(已加 onrestart)
```

## 当前调试目标（已进行到一半）

**问题**：Android 的 Wayland 版 composer HAL（`vendor.hwcomposer-2-1`）连不上 Weston 提供的
`/run/user/1000/wayland-0` socket，报 `Couldn't open Wayland display`，导致 SurfaceFlinger 起不来、
界面无法渲染。

**已确认**：
1. Weston 是活的，socket 存在且权限 `0666`，musl 侧 `wayland-info` 能连上同一个 socket。
2. `system.img` 内 `init.waydroid.rc` 已用 `override` + `setenv` 注入
   `WAYLAND_DISPLAY=/run/user/1000/wayland-0` 与 `XDG_RUNTIME_DIR=/run/user/1000`
   （绝对路径，用于绕过 libwayland 对 XDG_RUNTIME_DIR 属主的严格校验）。
3. `/run/user/1000` 当前设为 `0777`，Weston 以 root 运行、composer 以 `user host` 运行。
4. 诊断探针 `aether-composer-probe.sh` 已注入 system.img（service 命令行指向它），
   但探针把日志写到 `/data/local/tmp/aether-probe.log`，而 `/data`(0751) 与
   `/data/local/tmp`(0771) 对 `host` 用户无写权限 → 日志静默丢弃，取不到。

**尚待确认（探针要改落点）**：composer 进程视角里的真实
环境变量 / uid / mount namespace / socket 可见性，以判定到底是
「环境变量未生效」「mount namespace 隔离」还是「时序（Weston 晚于 composer 起）」。

**注意**：Weston 镜像在 `aether-start-weston.sh` 里是 `-o ro` 只读挂载，不可写；
9p 共享目录（hostshare，宿主 `/workspace/aether-linux/hostshare`）是更优的诊断日志落点。

## 关键命令回顾

```bash
# 启动(只运行不构建镜像; 吃现成的 system.img/vendor.img/data.img/weston-rootfs.img)
cd /workspace/aether-linux/android && bash boot-android.sh

# 实时看引导日志
tail -f /workspace/aether-linux/android/android-boot.log

# 从 data 镜像取文件(需先 kill QEMU 并 e2fsck)
debugfs -w -R "dump /local/tmp/aether-probe.log /tmp/probe.log" aetheros-android-data.img
```
