# AetherOS-Phone

AetherOS 是一个从零构建的移动操作系统：**ARM64 Linux 6.6.70 内核 + 完全自有的用户态**，
不含任何 AOSP 组件，也不跑 ART。手机上的交付形态是 **Android App 内嵌 QEMU 软件模拟跑客机**——
无需 root、无需解锁 bootloader，在普通安卓手机上就能运行一部独立的 Linux 手机系统。

它是 postmarketOS 的同族（真 Linux 手机系统），但把 AOSP 那套「系统服务如何切分」的工程
经验整套搬了过来：自有 init / 服务监管 / 合成器 / SystemUI / 包管理 / 沙箱 / 日志·崩溃·看门狗。
详细的谱系与借鉴清单见 [`docs/AETHEROS-OS-LINEAGE.md`](docs/AETHEROS-OS-LINEAGE.md)。

> 一部手机，不是一个窗口：默认竖屏 720×1600（1080×2400 手机的原生 9:20 比例），VNC 里
> 铺满整块屏幕；开机先锁屏、点按解锁进应用网格主屏；桌面是实拍照片壁纸 + iOS 风格液态玻璃。

---

## 一、这是什么（30 秒版）

| 项 | 说明 |
|---|---|
| 内核 | 自编译 Linux 6.6.70（aarch64），含 virtio 块设备 / 网络 / 9p / DRM-KMS / evdev / seccomp 等子系统 |
| 用户态 | PID 1 `aether-init`、守护 `aetherosd`、合成器 `aether-compositor`、SystemUI `aether-systemui`、包管理 `.aepkg`、沙箱、logd/crashd/watchdog |
| 图形 | 裸 `ioctl` 原子 KMS（不依赖 libdrm）+ 自绘软件光栅化器（无第三方图形库），多进程经 `SCM_RIGHTS` 共享内存上屏 |
| 包管理 | `.aepkg`：ed25519 全内容签名、发布公钥钉死在二进制里（比 v1 APK 签名更严） |
| 安全 | 应用以 uid/gid 1000、能力集清零、私有 mnt ns、seccomp BPF 允许列表、NoNewPrivs 运行 |
| 升级 | A/B 双槽 + initramfs 选择器 + `aether-ota`，切槽前校验目标槽（防砖） |
| 手机交付 | Android 自包含终端 APK（`aetheros-terminal.apk`），内嵌 QEMU 11.1.2 与 WebView 前端 |
| 验证 | 全部基于 QEMU 真机级断言链，非模拟；CI 两条 GitHub Actions 工作流覆盖编译到全链路启动 |

---

## 二、架构与组件对照

客机系统（跑在内核上的 Linux 用户空间）的核心组件：

| 组件 | 源码 | 角色 | 状态 |
|---|---|---|---|
| `aether-init` | `aether-init.c` | 原生 PID 1：挂载、声明式服务清单、依赖拓扑、崩溃防抖监管、信号契约 | ✅ |
| `aetherosd` | `aetherosd.c` | 系统守护进程：读 `/proc`/`/sysfs` 实时采集，UDS `/run/aetheros.sock` 暴露 AetherBus 契约 | ✅ |
| `aether-compositor` | `aether-compositor.c` + `aether-drm.c` + `aether-render.c` | DRM/KMS 原子提交 + 自绘合成器 + WM + IPC（`/run/aetheros-comp.sock`） | ✅ |
| `aether-systemui` | `aether-systemui.c` | 状态条 + Launcher，锁屏→启动器两态状态机 | ✅ |
| `aether-pkg` | `aether-pkg.c` | `.aepkg` 验签 / 安装 / 索引（宿主与客机同源） | ✅ |
| `aether-sandbox-run` | `aether-sandbox.c` | 应用运行时入口：施加与 init 服务相同的沙箱后 exec | ✅ |
| `aether-logd` / `aether-crashd` / `aether-watchdog` | 对应 `.c` | 序列化日志 / 崩溃报告 / 软看门狗心跳 | ✅ |
| `aether-ota` | `aether-ota.c` | A/B 槽位管理与恢复 | ✅ |
| `aether-settings` / `aether-clock` / `aether-updater` | 对应 `.c` | 系统应用：设置 / 时钟 / 系统更新 | ✅ |
| `aether-hal-android` | `aether-hal-android.c` | 安卓宿主 HAL 后端：经 9p 把真机电量/网络喂给客机（诚实降级） | ✅ |

手机宿主侧（Android App）的关键模块见 [`docs/PHONE-V18-ARCHIVE.md`](docs/PHONE-V18-ARCHIVE.md) 与安全修复记录
[`docs/PHONE-V18-ROOT-CAUSE.md`](docs/PHONE-V18-ROOT-CAUSE.md)。

---

## 三、目录布局

```
aether-linux/
├── README.md                  # 本文件：项目总纲（接手必读）
├── AGENTS.md                  # 接手者指南：环境/构建/测试/已知坑/待办
├── CLAUDE.md                  # 指向 AGENTS.md 的简短入口
├── LICENSE                    # MIT
├── .gitignore                 # 排除 GB 级目录/可重建产物/私钥/证据
├── .gitattributes             # 大二进制（内核/initramfs/.so/APK）走 Git LFS
├── bootstrap.sh               # 一键装齐工具链 + LFS
│
├── *.c / *.h                  # 客机用户态全部源码（约 50 个文件）
├── aetheros-kernel-6.6.70-aarch64.Image      # LFS：客机内核（boot.sh 用）
├── aetheros-kernel-6.6.70-aarch64-android.Image  # LFS：安卓宿主内嵌内核
├── aetheros-initramfs.cpio.gz                 # LFS：initramfs（A/B 选择器用）
├── aetheros-rootfs.ext4       # 注意：被 .gitignore 排除，由 make_rootfs.sh 重建
├── boot.sh                   # 宿主机一键启动（内核 + rootfs + 网络 + 9p）
├── make_rootfs.sh            # 重建 rootfs.ext4（参数化路径，CI 同路径）
│
├── ci/                       # CI 资产（脚本 + 本地全链路模拟）
│   ├── build.yml / boot-smoke.yml   # 见 .github/workflows/ 的镜像
│   ├── boot-smoke.py / ab-smoke.py  # QEMU 断言链
│   ├── ipc-contract-smoke.sh / check-artifacts.sh / run-ci.sh
│   ├── build-busybox.sh / busybox-aarch64.config
│   ├── make_ab.sh / ab-initramfs-init.sh   # A/B 镜像组构建
│   └── test-signing.key            # ⚠️ 私钥，已被 .gitignore 排除
├── .github/workflows/       # GitHub Actions：build.yml + boot-smoke.yml
├── assets/                  # 壁纸源（PPM 走 LFS）
├── tools/                   # 字体生成器（gen_font.py / gen_cjk_font.py）
├── third_party/tweetnacl/   # ed25519 签名实现（包管理依赖）
│
├── docs/                    # 工程文档（见 docs/README.md 索引）
└── dist-phone/              # 手机交付树（镜像包 + 终端 APK + App 壳）
    ├── phone-boot.sh / build-phone-tarball.sh / build-phone-app-apk.sh
    ├── terminal-apk/        # 自包含终端 APK 源码 + lib/*.so（LFS）+ .apk（LFS）
    ├── apk-src/             # aether-phone-app WebView 壳源码
    └── README.md / README-phone.md   # 用户面向的安装与使用说明
```

---

## 四、当前进度（已交付）

| 阶段 | 内容 | 验证 |
|---|---|---|
| Phase 1 | 真 Linux 内核 + 总线/服务层（aether-init / aetherosd 真读内核） | 实跑取证 |
| P0-6 | 裸 ioctl 原子 KMS + 自绘合成器（绕过 Android 兼容层） | 42/42 断言 |
| P1-1a | 多进程窗口系统：客户端经 `SCM_RIGHTS` 共享内存上屏 | 43/43 断言 |
| P1-1b | 中文 16×16 点阵字体（构建期 bake 进头文件，来源可溯） | 44/44 断言 |
| P1-2a | evdev 真实键盘（EVIOCGBIT 三重识别 + QMP send-key 焦点切换） | 真 QEMU |
| P1-2b | 安全沙箱：非 root + 零能力 + mnt ns + seccomp BPF | S0-S4 断言 |
| P1-2c | OTA A/B 双槽 + recovery（切槽前校验，防砖） | 32/32 断言 |
| P1-2d | 多 plane 硬件合成枚举与如实上报 | J17 断言 |
| P1-2e | virgl GPU 能力检测（诚实检测，未加速） | 代码路径覆盖，3D 实跑 SKIP（宿主无 render node） |
| P2-11 | `.aepkg` 签名格式 + ed25519 钉死发布密钥 + 开机/运行时安装 | 负向测试覆盖 |
| P3-14 | SystemUI：状态条 + Launcher + 锁屏/启动器状态机 | CI 覆盖 |
| P4-1 / P5 | 系统应用：设置 / 时钟 / 系统更新 | CI 覆盖 |
| phone-v18.1 | 终端 APK 三处重连根因修复（encode 移位 / app.js 多矩形 / SSE 增量） | 见 docs/PHONE-V18-ROOT-CAUSE.md |

**CI 总账（2026-10-18 沙箱实跑，0 FAIL）**：85 项 QEMU 实跑断言（含 2 项 virgl 因宿主硬件 SKIP）
+ 13 项宿主 IPC 契约断言 + 4 项工件断言。

---

## 五、快速开始

### 5.1 环境

```bash
bash bootstrap.sh          # 装交叉工具链 + QEMU + e2fsprogs + Git LFS 并拉取大二进制
```

### 5.2 构建用户态并生成 rootfs

```bash
# 1) 交叉编译静态 ELF（对应 make_rootfs.sh 的 AETHER_* 变量）
aarch64-linux-gnu-gcc -static -O2 -o aether-init-aarch64 aether-init.c
aarch64-linux-gnu-gcc -static -O2 -DAETHEROS_BUILD='"local"' -o aetherosd-aarch64-static aetherosd.c
aarch64-linux-gnu-gcc -static -O2 -I. -o aether-compositor aether-compositor.c aether-drm.c aether-render.c
# 其余组件（pkg / sandbox-run / systemui / settings / clock / updater / logd / crashd /
# watchdog / ota / hal-android）同法交叉编译到 build/ 或用 -o 指定到 make_rootfs.sh 期望路径

# 2) 构建 busybox 1.36.1（aarch64，版本化配置保证确定性）
ci/build-busybox.sh "$HOME/linux-build"        # 产出 $HOME/linux-build/busybox-1.36.1/_install

# 3) 生成 rootfs.ext4（参数化路径，CI 与本地同一条路径）
./make_rootfs.sh
```

### 5.3 启动客机

```bash
./boot.sh                 # Ctrl-A 然后 X 退出
# 串口可见 PID1=aether-init、服务拓扑、IPC 回环、图形栈取证
```

### 5.4 手机交付

```bash
# 客机镜像包（kernel Image + ext4 rootfs）
cd dist-phone && bash build-phone-tarball.sh

# 自包含终端 APK（需 Android SDK build-tools 35.0.0 + JDK）
cd dist-phone/terminal-apk && bash build-apk.sh
```

---

## 六、给接手者的指引（必读）

接手本仓库后，按以下顺序吸收信息，可在一小时内建立完整心智模型：

1. **`AGENTS.md`** —— 环境、每个组件的构建命令、测试与 CI 运行方式、已知坑、待办清单、关键文件索引、代码约定。这是实际操作手册。
2. **`docs/README.md`** —— 全部工程文档的索引与阅读顺序。
3. **`docs/AETHEROS-OS-LINEAGE.md`** —— 先定性「AetherOS 属于哪一类」，再看它从哪些开源系统借鉴了什么、明确不抄什么。
4. **`docs/README-userland.md`** —— 用户空间各阶段的落地细节、实跑证据、踩坑记录、诚实边界（最厚的一份，按需查阅）。
5. **`docs/GRAPHICS-STACK-AUDIT.md`** —— 图形栈的工程审计（ioctl 契约 / 已修缺陷 / 验证证据）。
6. **`docs/AETHEROSD-PROTOCOL.md`** —— `aetherosd` 的 IPC 契约（`aetherosd.state/1`），对接实时状态时的规范来源。
7. **`docs/PHONE-V18-ROOT-CAUSE.md` + `docs/PHONE-V18-ARCHIVE.md`** —— 手机端最近一次封存的来龙去脉。

---

## 七、诚实边界（不粉饰）

- **无基带/RIL**：通话、短信、移动数据不可用（无 modem 驱动与电话栈）。
- **无安卓应用兼容**：不跑 APK（需要 binder + ART + libhybris 级工程）；`aether-hal-android` 是宿主数据桥，不是 APK 运行时。
- **CPU 软件渲染**：无 GPU 合成加速；virgl 3D 命令流从未在本沙箱真实跑过（宿主无 render node，已如实 SKIP）。
- **字体为精选子集**：ASCII 8×16（95 字符）+ CJK 16×16（59 字），子集外汉字渲染为 tofu 方框。
- **单设备**：QEMU `virt` 平台；真机移植需要 BSP（内核/rootfs 分层已为其留好位置）。
- **短连接 IPC 模型**：合成器每命令后 `close`，客户端须每命令独立连接（已写入文档与 demo，非缺陷）。

---

## 八、许可证

MIT —— 见 [`LICENSE`](LICENSE)。
