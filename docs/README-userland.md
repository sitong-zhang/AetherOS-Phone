# Phase A —— AetherOS 服务层落到真 Linux 内核用户空间

把 Phase 1 的「双后端抽象」兑现：**AetherOS 的 HAL 现在真的从 Linux 内核读数据，不再依赖 `android.*`**；并把 `AetherSystem + AetherBus` 的对应物做成 **Linux 原生守护进程**，在 QEMU 已验证的真内核上实跑。

## 零、`aether-init`：AetherOS 原生 PID 1（2026-10-18，P0-2）

此前 rootfs 的 PID 1 是 busybox init（inittab 只有 sysinit/respawn/ctrlaltdel/shutdown 四动作）。现由 `aether-init.c` 取代：

| 能力 | 实现 |
|---|---|
| 虚拟文件系统 | 原生挂载 proc / sysfs / devtmpfs / devpts / run / tmp（不再依赖 `mount -a`） |
| 服务清单 | `/etc/aether/services` 声明式清单：`service / exec / depends / restart / oneshot` |
| 依赖拓扑 | 按依赖序启动（network → aetherosd → bootdiag → getty 实测有序） |
| 崩溃监管 | daemon 崩溃按 `restart=always` 拉起；10s 内崩溃 5 次判「反复崩溃」放弃（防抖） |
| 信号契约 | SIGTERM/SIGINT=优雅关机，SIGUSR1=重启，SIGHUP=恢复（写 `/run/aether-recovery.request`） |
| PID 1 职责 | 收养并回收全部孤儿进程；`prctl(PR_SET_NAME)` 自证 `comm=aether-init` |

```sh
aether-power poweroff   # 向 PID 1 发 SIGTERM，由 init 逆序停服务后 PSCI 关机
aether-power reboot     # SIGUSR1
aether-power recovery   # SIGHUP（A/B 槽位切换属 Phase 2，此处只完成信号契约+标记）
```

**如实记录的边界**：recovery 目前只做「优雅停机 + 写请求标记」，不冒充槽位切换；服务经 `/bin/sh -c` 执行（与 busybox inittab exec 语义一致，便于复用 rc 脚本）；已登录的交互 shell 按 POSIX 语义忽略 SIGTERM，由 init 在 10s 宽限后 SIGKILL——与 systemd/busybox init 行为一致。

## 一、`LinuxHalBackend`：从 TODO 桩 → 真读 Linux 内核

原先 `LinuxHalBackend` 的 25 个方法全部返回占位的 "unknown"（诚实但不做事）。现已改为 **纯 Kotlin + Java IO 读 procfs / sysfs**：

| 能力 | 真实数据源 |
|---|---|
| `memory()` | `/proc/meminfo`（MemTotal / MemAvailable / Cached） |
| `cpu()` | `/proc/cpuinfo`（Hardware、processor 计数、model name） |
| `volumes()` | `/proc/mounts` + 真实文件系统容量 |
| `battery()` | sysfs `power_supply` 的 `uevent`（容量/状态/健康/温度/电压） |
| `display()` | framebuffer `virtual_size`（**刷新率故意留 0=未知，不伪造 60Hz**） |
| `sensors()` | IIO 设备 + hwmon 芯片 + thermal 温区 |
| `cameras()` | V4L2 视频节点 + LED flash class |
| `network()` | `/proc/net/dev` 字节计数，速率由相邻两次真实采样差值算出 |
| `device()` | 设备树 model + `/etc/os-release` + hostname |
| `uptimeSeconds()` / `kernelVersion()` | `/proc/uptime` / `/proc/version` |
| 无线电状态 | rfkill 的 soft/hard block 位 |

**零 `android.*` 调用**（`Context` 只出现在接口签名里，实现完全不用它）。
读不到权威值的一律返回 Unknown 而不是编造：全局音频音量、Wi-Fi SSID、正在播放的媒体——这些内核视图里本就不存在。

> 因为 Android 自身就跑在 Linux 内核上，这些路径在两个后端上通用；差别只在于"谁来产出这些数字"。

## 二、`aetherosd`：Linux 原生系统守护进程（真跑起来的那个）

`aetherosd.c` —— AetherOS 服务层的原生落地，对应 `AetherSystem + Linux HAL + AetherBus`：

- **采集**：`/proc/version`、`/proc/meminfo`、`/proc/cpuinfo`、`/proc/uptime`、`/proc/mounts`、`/proc/net/dev`、`/etc/os-release`、virtio 磁盘节点、9p 挂载
- **IPC**：Unix domain socket `/run/aetheros.sock` —— 这就是 AetherBus 在 Linux 上的形态
- **状态落盘**：`/run/aetheros.state`
- **三种模式**：常驻服务（默认）、`-1` 单次采集后退出（init 早期取证）、`-c` 客户端向总线取状态

### 踩过的坑（已修，如实记录）
初版用了 `statvfs()` 统计卷容量 + `readdir` 遍历 `/proc`，**结果进程永久阻塞**——`statvfs` 撞上不可达/fuse 挂载点就不返回，而 SIGTERM 被信号处理函数吃掉导致 `timeout` 也杀不掉。
现版本**只对常规小文件做 fopen + fgets**，必然返回；卷容量改用 `/proc/mounts` 的挂载类型，不再 statvfs。

**第二个坑（2026-10-18，由 aether-init 监管取证暴露）**：`signal(SIGTERM, ...)` 在 glibc 是 BSD 语义（带 SA_RESTART），`accept()` 被信号打断后自动重启，aetherosd 收到 SIGTERM 也退不出——init 只能等 10s 后 SIGKILL。已改用 `sigaction` 且 `sa_flags=0`（慢速系统调用返回 EINTR），并在 `accept()` 返回后立即检查退出标志。

## 三、实跑验证（真 QEMU，非模拟）

启动日志见 `boot-evidence.log`，三次快照经不同通道取得：

```
# 1) 单次采集
  uptime        : 2 s        memory: total 414428 kB / available 396020 kB
  root volume   : / (ext4)   cpus: 2   mounts: 8   net ifaces: 2
  virtio disk   : present (/dev/vda)
  9p share      : mounted at /mnt/share (9p)

# 2) 常驻守护进程
  uptime        : 3 s        memory available 395924 kB
  aetherosd: IPC socket online at /run/aetheros.sock

# 3) aetherctl -c 经 IPC 总线取状态（证明 IPC 真实工作）
  uptime        : 5 s        memory available 395248 kB
```

三次 uptime 递增（2→3→5s）、可用内存真实变化（396020→395924→395248 kB），全部来自**我们自己在 QEMU 里启动的那颗 Linux 6.6.70 内核**。

## 四、文件清单

- `aether-init.c` / `aether-init-aarch64` —— AetherOS 原生 PID 1（源码与静态二进制）
- `aetheros-kernel-6.6.70-aarch64.Image` —— 交叉编译的 aarch64 真内核
- `aetheros-rootfs.ext4` —— 真实 ext4 根文件系统（PID 1 = aether-init，含 aetherosd 与服务清单）
- `aetherosd.c` / `aetherosd-aarch64-static` —— 原生守护进程源码与静态二进制
- `make_rootfs.sh` —— 重建 rootfs 的脚本（含 `/etc/aether/services` 清单生成；输入路径全部可用环境变量覆盖，见 §六）
- `boot.sh` —— 一键启动（`sudo apt-get install qemu-system-arm` 后 `./boot.sh`）
- `boot-evidence.log` —— busybox init 时代的启动取证（历史存档）
- `aether-init-boot-evidence.log` —— aether-init 首启取证（PID 1 身份 / 依赖序 / IPC / 崩溃自恢复 / 优雅关机）
- `aether-init-flap-evidence.log` —— 防抖监管取证（连续 5 次崩溃 → 放弃重启）
- `hostshare/` —— 9p 宿主共享目录（guest 内 `/mnt/share`）
- `ci/` —— CI 资产（工作流脚本 + 本地全链路模拟，见 §六）
- `ci-evidence.log` —— 本地 CI 全链路实跑证据（2026-10-18，8 阶段全绿）
- `boot-smoke-summary.txt` —— QEMU 启动冒烟断言汇总（29/29 通过）

## 五、诚实边界

- **Kotlin/Native 走不通**：`~/.konan` 不存在，GitHub Releases 连 HEAD 都返回空，500MB 的 Kotlin/Native bundle 拉不下来。所以现有 Kotlin 服务层**没有**被交叉编译到 Linux——改用「Kotlin 侧真读 Linux + C 侧做原生守护进程」两条腿落地。
- **原 Compose UI 仍未移植**：它是 Android 应用，跑不了裸 Linux。要承载它要么上 Android 兼容层，要么用 Linux 原生 toolkit 重写（这是下一步的方向选择题）。
- `aetherosd` 目前读的是「最小且必然返回」的子集（`statvfs` 等已剔除以保持可靠性），尚未覆盖音量/SSID/媒体会话这类本就无内核读数的能力。

## 六、CI（2026-10-18，Phase 1b）

两条 GitHub Actions 工作流 + 一套可本地复跑的脚本资产。设计原则：**断言链只认真实输出，所有期望字面值取自本仓库的取证日志**，不猜、不放宽。

| 工作流 | 覆盖 | 内容 |
|---|---|---|
| `.github/workflows/build.yml` | 秒级反馈 | apt 交叉工具链 → 静态编译 aether-init/aetherosd → `check-artifacts.sh`（ARM64 静态 ELF 断言）→ `ipc-contract-smoke.sh`（宿主真 UDS + JSON 契约 13 项断言） |
| `.github/workflows/boot-smoke.yml` | 分钟级全链路 | busybox 交叉编译 → rootfs 构建 → `boot-smoke.py`（QEMU 启动 + 29 项断言链）；每日 UTC 18:00 巡检 |

### 脚本资产（`ci/`）

| 文件 | 职责 |
|---|---|
| `check-artifacts.sh` | 断言产物为 ARM aarch64 静态 ELF（`file` + `readelf` 双重证据：无 dynamic section、e_machine=AArch64） |
| `ipc-contract-smoke.sh` | 宿主 IPC 契约冒烟：负例（无守护进程必须诚实失败）/ socket mode 0660 / JSON 字段 / uptime 递增（活数据）/ `-1` 状态文件 / 退出后 socket 清理 |
| `boot-smoke.py` | QEMU 启动断言链：启动取证 → 登录 → PID1 身份 → 服务拓扑 → IPC 准入与契约 → 9p → **kill -9 崩溃自恢复（新 PID 与 init 报告一致）** → 优雅停机（QEMU 自行退出） |
| `build-busybox.sh` | busybox 1.36.1 交叉编译；使用版本化已知良好配置 `ci/busybox-aarch64.config` |
| `run-ci.sh` | 本地全链路模拟（无 GitHub 环境复演两条工作流）：`./ci/run-ci.sh [--skip-core] [--skip-boot]` |

### 实跑证据（2026-10-18，沙箱）

`ci-evidence.log` 记录了一次完整本地 CI：**8/8 阶段通过**——core（wrapper + JDK20 + 169 项测试 0 失败）、交叉编译、工件断言、IPC 契约（13 项）、busybox、rootfs、boot-smoke（29 项）。`boot-smoke-summary.txt` 为断言明细。

### 踩坑与修复（如实记录）

1. **busybox 1.36.1 × Linux 6.8 UAPI 头文件**：默认 defconfig 开启 `CONFIG_TC`，而 `networking/tc.c` 引用的 `TCA_CBQ_*` 已随 CBQ qdisc 从 6.8 头文件中移除——必然编译失败。修复：关闭 TC（rootfs 不使用该 applet）并把**终态配置入库版本化**，`oldconfig </dev/null` 保证零交互确定性。
2. **getty 的 exec 链**：busybox getty → login → sh 是同一 pid 的 exec 链，`ps` 中 comm 最终是 `sh`。初版断言写 `comm=getty` 失败（28/29）；修正为锚定 init 拉起的服务 pid 存活且 PPID=1——不断言会变换的中间形态，只断言不变量。
3. **沙箱代理截断大文件**：`services.gradle.org` 的 137MB 发行包下载在 ~38MB 处停滞（gh 静默"成功"、`unzip -t` 证伪）。本仓库的 Gradle Wrapper 资产（jar/gradlew）经 SDKMAN 正版安装交叉验证（sha256 逐字节一致），distributionUrl 指向官方地址并**钉死官方发布 sha256**，由 CI 网络完成真实下载与校验。

## 七、P0-6 图形栈：DRM/KMS 原子提交 + 自绘合成器（2026-10-18）

此前 AetherOS 的图形输出走「Android SurfaceFlinger → Weston(Wayland) → 合成器 HAL」的兼容层链路（见 SOURCE-README 的调试目标）。本轮**绕过整个 Android 兼容层**，在原生 Linux 用户空间直接驱动显示硬件：方案 A——**裸 `ioctl` 实现 DRM 原子 KMS（不依赖 libdrm）+ 自绘软件光栅化器（无第三方图形库）**。

> 选型依据：APK 只是 Simulator / Reference Platform，AetherOS 的底座是 Linux 内核。图形栈不应重建内核已成熟的 KMS 子系统，也不应引入 Wayland/Weston 这类需要完整用户空间会话管理的依赖。直接吃内核 `drm` 设备是最小、最诚实的路径。

### 架构分层

| 层 | 文件 | 职责 |
|---|---|---|
| 内核 | `aetheros-kernel-6.6.70-aarch64.Image` | 增量开启 `DRM` / `DRM_KMS_HELPER` / `DRM_GEM_SHMEM_HELPER` / `DRM_VIRTIO_GPU` / `DRM_VIRTIO_GPU_KMS` / `VIRTIO_DMA_SHARED_BUFFER`（恰 7 项 CONFIG） |
| KMS 驱动层 | `aether-drm.h` / `aether-drm.c`（669 行） | 裸 ioctl 枚举 connector/crtc/plane、两轮式 property 读取、dumb buffer 双缓冲、原子提交 modeset 与 page-flip |
| 光栅化层 | `aether-render.h` / `aether-render.c`（136 行）+ `aether-font.h`（8×16 位图字体） | XRGB8888 帧缓冲填充/矩形/垂直渐变/文本/窗口/桌面 |
| 窗口管理 + IPC | `aether-compositor.c`（518 行） | 最小 WM（窗口表 + 焦点）、UDS IPC `/run/aetheros-comp.sock`（schema `aethercomp.state/1`）、FNV-1a-64 帧哈希、PPM 截图 |

### 文件清单（图形栈）

- `aether-drm.h` / `aether-drm.c` —— 裸 ioctl 原子 KMS 层（接口契约 + 状态机）
- `aether-render.h` / `aether-render.c` —— 软件光栅化器（无第三方图形库）
- `aether-font.h` —— 8×16 位图字体（DejaVu Sans Mono 经 `tools/gen_font.py` 二值化生成，可再生产）
- `aether-compositor.c` —— WM + IPC（静态 ARM64 二进制 598168 B）
- `tools/gen_font.py` —— 字体生成存档（证明字体来源可溯，非手绘猜测）

### 实跑验证（真 QEMU，42/42 断言全绿）

`boot-smoke.py` 的 **J 段 12 项图形断言**全部通过：

```
J1  /dev/dri/card0 字符设备存在（内核 DRM 子系统已加载）
J2  /run/aetheros-comp.sock mode 0660 (srw-rw----)
J3  合成器进程运行且 PPID=1（由 aether-init 监管）
J4  IPC schema=aethercomp.state/1
J5  mode=1280x800（KMS 当前模式）
J6  windows>=2（WM 实际管理窗口）
J7  hash 为 16 位十六进制（FNV-1a-64 帧指纹）
J9  空闲帧哈希稳定（3s 间隔两次一致 → 无状态变化不重绘）
J10 focus 切换后帧哈希变化（真渲染路径，非空翻页）
J11 screenshot 落盘 PPM 且尺寸=KMS 模式（1280x800）
J12 close 后 windows 递减（WM 真删除窗口）
```

**视觉铁证**：经 9p 取回的 `aether-comp-shot.ppm`（1280×800）目检为真实桌面——深蓝垂直渐变背景、左上 `aetherosd` 窗口（focused 态：亮青边框+焦点圆点）、中央 `bootdiag` 窗口（非 focused：暗边框），8×16 字体清晰渲染出 `[OK] virtio-gpu KMS active` / `[OK] compositor serving IPC`。像素采样非黑比例约 18%，标题栏/正文/focus 高亮三者颜色各不相同，证明是**真实逐像素绘制**而非占位画面。

### 踩过的坑（已修，如实记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | GETRESOURCES/GETCONNECTOR 报 `Bad address`(EFAULT) | 内核两轮式 ioctl：第一轮 `count=0` 探测数量，第二轮填指针数组；**任何 `count>0` 的指针数组漏填即 EFAULT**（漏了 `encoder_id_ptr` / `props_ptr` / `prop_values_ptr`） | 全部指针数组配套 alloc 并填充 |
| 2 | OBJ_GETPROPERTIES 报 `ENOENT` | 误用自定义结构体当 ioctl 参数传给内核（布局与 UAPI `drm_mode_obj_get_properties` 不同，纯 UB） | 直接用内核 UAPI 结构体两轮式 |
| 3 | 初期「no usable primary plane found」 | `first_modeset` 假设 prop id 连续（`p_crtc = p_fb + 1`）并把 crtc 名字写入相邻栈变量 | 改用显式数组按名字顺序接收 prop id |
| 4 | plane type 判据永远不匹配 | **本地补齐的 `DRM_PLANE_TYPE_*` 常量数值抄错**：写成 PRIMARY=0/CURSOR=1，而内核 `enum drm_plane_type` 实为 OVERLAY=0/PRIMARY=1/CURSOR=2；virtio-gpu 的 primary plane 回填值 1 被误判 | 按内核 `drm_plane.h` 真实 enum 修正（含 ABI 出处注释） |
| 5 | J10 后屏显全黑（截图像素全零） | 双缓冲 `fb.px` 启动时绑定 `map[0]` 后永不更新；`render_frame` 始终画旧 back buffer，而 `present` 翻的是已切换的 `fb_id[1]` | `render_frame` 每次绘制前把 `fb` 重新绑定到当前 back buffer |
| 6 | 客户端写命令全 `unknown command` | `client_mode(argv[2])` 只取第一个参数（如 `focus 1` 的 `1` 被丢弃），服务端 `strncmp(req,"focus ",6)` 前缀匹配不上 | 客户端 join 全部参数后再发 |

### 诚实边界

- **软件渲染，非 GPU 加速**：当前光栅化为 CPU 逐像素（XRGB8888 dumb buffer）。virtio-gpu 仅用作 KMS 显示控制器（原子 modeset + page-flip），未启用 virgl/3D、未走 GEM 导入共享。这是 Reference Platform 的合理取舍，不冒充硬件加速合成。
- **单 plane 合成**：只用 virtio-gpu 的 primary plane（cursor plane 因内核 ATOMIC cap + `DRIVER_CURSOR_HOTSPOT` 过滤策略被有意不报，符合预期），WM 自行做窗口叠加；不支持多 plane 硬件合成/overlays。
- **字体为 ASCII 8×16（95 可打印字符）+ CJK 16×16 精选子集（59 字，P1-1b）**：中文已可上屏（UTF-8 混合渲染），但 CJK 是覆盖 UI 字符串的精选子集而非完整字体——子集外汉字渲染为 tofu 方框；构建期 bake 入头文件，guest 零字体依赖。
- **evdev 输入未接入**：合成器预留了 Tab/Esc 的 poll 分支，但真实键盘事件走 evdev 的采集尚未实接（窗口焦点切换目前由 IPC 命令驱动，而非物理按键）。
- 图形栈工程审计报告见 [`GRAPHICS-STACK-AUDIT.md`](./GRAPHICS-STACK-AUDIT.md)。

## 八、P1-1a 多进程窗口系统：客户端经 SCM_RIGHTS 共享内存上屏（2026-10-18）

P0-6 的合成器只自绘两个固定 seeded 窗口（`aetherosd` / `bootdiag`），客户端只是「控制命令」（focus / close / screenshot）。这能证明 KMS + 自绘合成器是真的，但**不能证明它是一个「多进程窗口系统」**——任何窗口只要换内容都得改合成器代码，等于假框架。

本轮把它补成真正的多进程窗口系统：**独立进程的应用通过 IPC 向合成器注册窗口，并把它自己的像素缓冲经 Unix socket 的 `SCM_RIGHTS` 把 fd 传过去、由合成器 mmap 后 blit 上屏**。这与 Wayland `wl_shm` 的共享内存机制是**同一个真实机制**（都是在一个 `sendmsg` 里用 `cmsg` 带 `SCM_RIGHTS` 传 fd），不是另起炉灶的自创协议。

> 选型依据：AetherOS 的窗口系统不应依赖 Wayland 协议栈（完整的会话管理、registry、xdg-shell 全家桶），但**共享内存上屏这条路是成熟且正确的**——Wayland 自己就是这么做的。因此取「真机制、最小实现」：直接裸用 `SCM_RIGHTS` 传 memfd，跳过 wl 协议层。

### 架构：一次客户端提交的数据流

```
aether-demo (独立进程)
  │  1. connect_comp_retry()  ──短连接──▶ /run/aetheros-comp.sock
  │  2. "create demo 220 140"         │  ipc_handle(): win_open(title,cw,ch)
  │     ◀── {"ok":true,"id":N}        │   注册客户端窗口，返回 id
  │  3. memfd_create("aether-demo",    │
  │       MFD_CLOEXEC|MFD_NOEXEC_SEAL) │
  │     ftruncate(CW*CH*4); mmap(MAP_SHARED)
  │  4. 逐像素画内容(红块/进度条)       │
  │  5. sendmsg("buf N\n" + cmsg       │  ipc_handle(recv_fd): 收 fd
  │       SCM_RIGHTS={memfd})  ──────▶ │    recvmsg → mmap 客户缓冲
  │  6. "commit N"  ─────────────────▶ │  win->has_shm=1; win->dirty=1
  │                                   │
  │                            compositor 主循环 render_frame()
  │                              aether_render_window(): 若 has_shm
  │                                blit 客户缓冲(预留边框/标题栏) → fb
  │                              aether_drm_present(): 原子页翻转上屏
```

要点：
- **窗口状态在合成器全局表里，跨连接保留**。合成器是「一连接一请求一应答后即 `close`」的短连接服务（与 `aethercompctl` 同款模型）。因此客户端「每命令一条独立短连接」，不可复用同一条 socket 发多条——这正是 P0-6 早期 `SIGPIPE` 崩溃的根因，已在 demo 端按此模型重写。
- **`SCM_RIGHTS` 传的是 fd 本身，不是像素**。合成器收到的是内核里那个 memfd 的新文件描述符，双方各自 `mmap` 同一块匿名共享内存；客户端改缓冲、合成器下次 blit 即见新内容——零拷贝。
- **`depends` 只保序不保就绪**。demo 的 `connect_comp_retry` 用 250ms 退避、最多 40 次重试，兜底「合成器 socket 尚未 bind」的窗口期（DRM 初始化需要时间）。

### 文件清单（P1-1a）

| 文件 | 职责 |
|---|---|
| `aether-demo.c`（6.6 KB，新建） | 示例原生应用：**独立进程**，证明多进程窗口系统。含 `connect_comp_retry` / `cmd_create` / `cmd_buf`(`SCM_RIGHTS`) / `cmd_commit`，动画 `ANIM_FRAMES=40` 帧后静止保留末帧 |
| `aether-compositor.c` | 新增 `create <title> <w> <h>` / `buf <id>` / `commit <id>` 三命令；accept 分支改用 `recvmsg` + `CMSG_FIRSTHDR` 提取 SCM_RIGHTS fd；窗口表新增 `shm`/`shm_fd`/`has_shm` 字段 |
| `aether-render.c` | `aether_render_window` 末尾新增 shm blit（标题栏+边框预留，无越界） |
| `make_rootfs.sh` | 拷贝 `/sbin/aether-demo`；服务清单新增 `service demo`（exec `/sbin/aether-demo`，depends compositor，restart always）；`bootdiag` depends 加 `demo` |
| `ci/run-ci.sh` | 新增 `cross-demo` 编译阶段；`check-artifacts` 传 4 个文件；rootfs 步骤加 `AETHER_DEMO` 参数 |
| `ci/boot-smoke.py` | J12 期望改为 `windows==2`；新增 J13（demo 进程 PPID=1）/ J14（list 含 `"title":"demo"`）/ J15（截图扫描红色特征像素，阈值 50） |

### 实跑验证（真 QEMU，43/43 断言全绿）

在 P0-6 的 42/42 基础上，P1-1a 新增/调整了 J 段断言，总断言数升至 **43/43（0 FAIL）**：

```
J12 close 后 windows==2（剩 bootdiag + demo，客户端窗口不受影响）
J13 示例应用进程运行且 PPID=1（独立进程上屏，证明多进程）
J14 compositor 管理 demo 窗口（客户端经 IPC 注册的 title=demo）
J15 demo 像素真实上屏（截图全屏扫描红特征像素 r>200 & g<100 & b<100，得 N>50）
```

**像素级铁证**：在 J11 已落盘 guest 的 `aether-comp-shot.ppm`（1280×800）上做全屏逐像素扫描（`boot-smoke.py` J15），命中 `r>200 & g<100 & b<100` 的红色特征像素 **>50 个**（实测上千）。该红色块是 demo 在它自己的 memfd 缓冲里画的 `0xFFFF3030`（明亮红），与 seeded 窗口的灰蓝色调完全不同；它出现在截图上，证明「**独立进程的像素 → SCM_RIGHTS memfd → 合成器 mmap+blit → KMS 页翻上屏**」这条链路是真实闭合的，而非合成器内部写死的画面。

### 踩过的坑（已修，如实记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 客户端复用同 socket 发多条命令 → `SIGPIPE` 退出(code=141)，init 反复重启，demo 窗口在 list 里累积(102/103/104…) | 合成器短连接模型：每请求后 `close(c)`；第二条写已关 socket 触发 SIGPIPE | 重写 `aether-demo.c` 为「每命令独立短连接」（与 aethercompctl 一致）；`signal(SIGPIPE, SIG_IGN)` |
| 2 | demo 反复重启退出，合成器 IPC 响应偶发被客户端读成 "create failed" | `ipc_reply_line` 只 `write` 不补 `\n`，客户端 `recv_line` 按 `\n` 切分读 EOF 误判 | 服务端补 `\n`；客户端 `printf("%s")` 不再重复换行 |
| 3 | `memfd_create` 被 6.6 内核拒绝/warn | 6.6 内核要求 `MFD_NOEXEC_SEAL`（或 `MFD_EXEC`）任一标志，否则内核拒绝 | `memfd_create("aether-demo", MFD_CLOEXEC | MFD_NOEXEC_SEAL)` |
| 4 | J9（空闲哈希稳定）被打破 | demo 持续 8fps 动画，3s 间隔哈希必然变 | 动画改为有限帧（`ANIM_FRAMES=40`）后 `usleep` 静止保留末帧：既证动态提交，又保证 idle 确定 |
| 5 | 编译告警 | compositor `tbuf[128]` 截断告警、demo 缺 `stdint.h`/`write` 返回值忽略 | `tbuf` 改 512、补 `stdint.h`、加 `if (write(...) < 0)`；全部零警告 |

### 诚实边界

- **短连接模型是刻意取舍，非缺陷**：合成器每命令后 `close`，客户端须每命令独立连接。它简化了服务端状态（无需维护长连接/鉴权/心跳），代价是每命令一次 `connect` 开销——对「启动期自报 + 低频提交」的嵌入式窗口场景可接受；高频提交场景（如每帧一提交的游戏）需要改成长连接，这是下一步路线。
- **零拷贝共享内存，非 GPU 合成**：客户端与合成器各自 `mmap` 同一 memfd，合成器 CPU blit 进 dumb buffer。未走 GEM 导入 / virgl；多进程的是「像素来源」，合成仍是软件叠加。
- **动画有限帧后静止**：为让 J9 空闲哈希稳定可断言，demo 在 40 帧后停画保留末帧，并非持续渲染。
- **无客户端协议版本协商**：`create/buf/commit` 为当前固定 schema，未做 capability 协商；客户端与服务端须同源构建（rootfs 同一次构建内一致）。
- P1-1a 工程审计与 SCM_RIGHTS 机制取证见 [`GRAPHICS-STACK-AUDIT.md`](./GRAPHICS-STACK-AUDIT.md) §8。

## 九、P1-1b 中文点阵字体：桌面中文状态行真实上屏（2026-10-18）

P0-6 / P1-1a 的合成器桌面、窗口标题、状态行全部是 ASCII（`aether_font_8x16`，95 个可打印字符）。一个「移动操作系统」的桌面如果连「图形合成器」「窗口系统」这几个中文字都画不出来，那它的 UI 栈在中文环境下是残疾的——这是 P1 阶段必须补的真实能力缺口，不是装饰。

本轮补上 **16×16 中文点阵字体子集**，让合成器能渲染 UTF-8 混合字符串（ASCII + CJK 同一行），并在桌面底部渲染真实中文状态行。字体**来源可溯**：直接由宿主机已安装的 **Noto Sans CJK SC**（真实 TTF 光栅化）经 Pillow 生成位图，而非手绘猜测；生成管线与既有 `aether-font.h` / `tools/gen_font.py` 同源，只是换了一个 CJK 子集。

> 选型依据：操作系统 UI 字体本就是「覆盖 UI 字符串的精选子集」，ASCII 字体也只含 95 个可打印字符，并非「完整 ASCII 码表」。因此 CJK 字体同样取「精选子集」是诚实且合理的范围声明——覆盖桌面/系统名/常用词用到的字即可（本次 59 字），要扩展只改 `CHARS` 重生成。不引入 FreeType 运行时（那会让合成器在 guest 里多依赖一个动态库 + 字体文件分发），而是在构建期把位图 bake 进 `.h`，guest 启动零字体依赖。

### 架构：UTF-8 混合渲染的数据流

```
构建期（宿主机，一次性）:
  tools/gen_cjk_font.py
    └─ 读 /usr/share/fonts/.../NotoSansCJK-Regular.ttc (index=2, SC 字面)
       └─ Pillow 对每个汉字在 16×16 单元内按墨迹包围盒居中光栅化
          └─ 每字 16 行，每行 16 位 → 拆成 2 字节（MSB=最左像素）
             └─ 输出 aether-font-cjk.h:
                  aether_cjk_cp[59]       升序 codepoint（二分查找表）
                  aether_cjk_glyphs[59][32]  每字 16×2 字节

运行期（guest 合成器每帧）:
  aether_render_desktop()
    └─ aether_render_text_utf8(fb, 40, 752, "AetherOS 图形合成器 窗口系统已就绪", 0xE6EDF3)
         ├─ UTF-8 解码（1/2/3 字节三类）逐字符:
         │    ASCII (cp<=0x7F)   → blit_glyph8  (8×16, aether_font_8x16)
         │    BMP 汉字 (3 字节)   → cjk_find(cp) 二分 → blit_glyph16 (16×16)
         │    缺字形              → 画 tofu 方框（上/下/左/右 4 条线）
         └─ 写入 dumb buffer → aether_drm_present() 原子页翻上屏
```

要点：
- **字体在构建期 bake 进头文件**，guest 运行时不读任何字体文件、不依赖 FreeType；合成的 `aether-compositor` 仍是单一静态二进制，符合「不重造、不添加 guest 运行期依赖」铁律。
- **CJK 字形按 codepoint 升序存储 + 二分查找**（`cjk_find`），O(log n)；与 ASCII 走完全不同的 blit 路径（8 宽 vs 16 宽），同一行里 ASCII 与汉字交错也能正确推进光标。
- **MSB = 最左像素**：与 `aether-font.h` 的 ASCII 字模约定一致（左移 `15-gx`），这样二进制字模既能被 C 直接索引，也能被 Python 工具往返验证。
- **桌面中文行固定坐标 (40, 752)**，位于 1280×800 屏幕的底部带，在所有窗口矩形（最高到 y≈216）之下，永不被窗口覆盖——这是 J16 能稳定断言的前提。

### 文件清单（P1-1b）

| 文件 | 职责 |
|---|---|
| `tools/gen_cjk_font.py`（新建，4.3 KB） | 来源可溯的 CJK 位图生成器：从 Noto Sans CJK SC `.ttc`（index=2）经 Pillow 光栅化，`CHARS` 去重后 59 字，输出 16×16 居中字模（MSB 最左、每字 32 字节） |
| `aether-font-cjk.h`（生成，21.8 KB） | `aether_cjk_cp[59]`（U+4E00 一 … U+98CE 风，升序）+ `aether_cjk_glyphs[59][32]`；全部非全空，缺失字 0 个 |
| `aether-render.h` | 新增声明 `aether_render_text_utf8()` / `aether_text_width_utf8()` |
| `aether-render.c` | 新增 `#include "aether-font-cjk.h"`、`AETHER_DESKTOP_CN` 字符串常量、`cjk_find()`（二分）、`blit_glyph8()`、`blit_glyph16()`、`aether_render_text_utf8()`（UTF-8 三类解码 + ASCII/CJK/tofu 分支）、`aether_text_width_utf8()`；`aether_render_desktop()` 末尾渲染中文状态行 |
| `ci/boot-smoke.py` | 新增 **J16**：扫描截图矩形 `x∈[112,312], y∈[752,768]` 的近白像素（`r,g,b > 180`），断言 `cn_px > 400`（字模点亮像素合计实测 959） |

### 实跑验证（真 QEMU，44/44 断言全绿）

在 P1-1a 的 43/43 基础上，P1-1b 新增 J16，总断言数升至 **44/44（0 FAIL）**：

```
J16 中文点阵字体真实上屏 —— 近白汉字特征像素 959 个（阈值 400，字模合计~959）
```

**像素级铁证**：boot-smoke 在已落盘的 `aether-comp-shot.ppm`（1280×800）上，只扫描桌面中文行所在矩形 `x∈[112,312], y∈[752,768]`，命中去底色的近白像素（`r>180 & g>180 & b>180`）**959 个**——精确等于 12 个汉字字模点亮的像素合计（阈值 400，留足防抖余量，不至于 flaky 又不致误判）。这些近白像素只可能来自 `AETHER_DESKTOP_CN` 那行 `0xE6EDF3` 的汉字笔画，证明「**UTF-8 字符串 → 二分查表 → 16×16 位图 blit → KMS 页翻上屏**」链路真实闭合，而非合成器里写死的 ASCII 画面。

**字模来源可溯性**：59 个字模全部来自 Noto Sans CJK SC 的真实光栅化，可往返验证。抽样 `中`(U+4E2D) 解码为「方框 + 居中竖笔」，与汉字「中」字形一致；`图`(U+56FE)、`窗`(U+7A97) 等同理。生成器 stderr 报告 `missing: none`，59 字无缺字、无全空字模。

### 踩过的坑（已修，如实记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 初版 CJK 生成器把 16 位行存成单个 `0xXXXX` 元素进 `uint8_t[32]` | 16 位值塞进 `uint8_t` 触发 `-Woverflow`（如 `0x7FFE`→254 截断），字模高位丢失、字形崩坏 | 每 16 位行拆成两个 `0x%02X` 字节：`(b>>8)&0xFF, b&0xFF`，共 32 字节/字；重编译 **0 warnings** |
| 2 | `aether-compositor` 交叉编译告警 | 复用既有 `-Wextra` 严格标志，新增 UTF-8 函数需严谨类型 | 显式 `uint32_t` 移位（`1u<<(15-gx)`）、无符号右移、缺失分支 `p++` 跳过；全零警告 |
| 3 | 中文行被窗口覆盖导致 J16 不稳定 | 早期 y 坐标若落在窗口矩形内，会被窗口 chrome 覆盖 | 固定 `(40, 752)` 底部带，确认所有窗口最高到 y≈216，永不重叠 |

### 诚实边界

- **精选子集，非完整 CJK 字体**：59 字覆盖桌面/系统名/常用词，不是 GB2312/Unicode 全汉字。任何不在此子集的汉字会渲染成 **tofu 方框**（已显式实现，而非乱码/崩溃），这是诚实的范围声明——要扩字符集改 `CHARS` 重跑生成器即可。
- **构建期 bake，非运行时 FreeType**：为保持 guest 静态单二进制、零字体依赖而刻意取舍；代价是字体不可在运行期切换/缩放（16×16 固定），需要 TTF 运行时再引入 FreeType + 字体分发。
- **无 Unicode 复杂形态**：UTF-8 解码只覆盖 1/2/3 字节（BMP），未处理 4 字节辅助平面（emoji 等）；非法前导字节直接跳过而非报错——对 OS UI 字符串足够，不是通用文本排版引擎。
- **MSB 最左约定固定**：与 ASCII 字模一致，二进制字模可被 Python 工具往返解码验证（已抽样校验 `中` 等）。
- P1-1b 工程审计与 CJK 位图生成取证见 [`GRAPHICS-STACK-AUDIT.md`](./GRAPHICS-STACK-AUDIT.md) §9。

## 十、P1-2a evdev 真实键盘：合成器经 EVIOCGBIT 识别真实输入设备（2026-10-18）

P0-6/P1-1x 的合成器只认「焦点切换命令」（`aethercompctl -c focus N`）这一条内部路径，没有一条按键是从**真实输入设备**进来的。一个连键盘都没有的图形栈不配叫窗口系统。本轮把 QEMU 的 **virtio-keyboard-pci** 真实设备接进合成器：内核 `CONFIG_VIRTIO_INPUT=y` 驱动它，合成器经 evdev  ioctl 识别并挂接，QMP `send-key` 注入的真实 Tab 键经这条路径切换窗口焦点。

> 选型依据：键盘识别不能靠「打开第一个 `/dev/input/event*` 就当它是键盘」——virt 平台上还躺着平台自带的 `gpio-keys`（电源键）。因此用 `EVIOCGBIT(0,...)` 确认设备支持 `EV_KEY`、再用 `EVIOCGBIT(EV_KEY,...)` 检查 `KEY_ENTER` 位（鼠标/电源键没有这个位），最后 `EVIOCGNAME` 记录设备名上日志。这是真实探测，不是路径猜测。

### 架构：一次真实按键的数据流

```
QEMU 侧（宿主）:
  QMP send-key "tab"
    └─ virtio-keyboard-pci（PCI 总线，QEMU 8.2 实测可用；
       mmio 变体 virtio-keyboard-device 在 -M virt 上不生成 DT 节点，已弃用）
       └─ virtio-input 队列 -> 客机内核 input 子系统
          └─ /dev/input/eventX（evdev）

客机侧（AetherOS 合成器）:
  open_evdev()
    ├─ openat(/dev/input, event*, O_NONBLOCK|O_RDONLY)
    ├─ EVIOCGBIT(0)      确认 EV_KEY 支持
    ├─ EVIOCGBIT(EV_KEY) 确认 KEY_ENTER 位（区分键盘 vs 鼠标/电源键）
    ├─ EVIOCGNAME        记录 "QEMU Virtio Keyboard" 上串口日志
    └─ poll(eventX) 循环 -> 读 input_event -> Tab: focus_next()
                          -> 重绘 -> KMS 页翻
```

要点：
- **设备识别三重证据**：`EV_KEY` 能力位 + `KEY_ENTER` 键位 + `EVIOCGNAME` 设备名，三者同时满足才认它是键盘；`gpio-keys` 只有 `KEY_POWER` 一类位，被正确排除。
- **与 WM 焦点路径闭环**：evdev 进来的 Tab 与 `aethercompctl -c focus N` 走同一个 `focus_next()`/`focus_set()`，K2 断言「注入前 focused != 注入后 focused」证明这条真实输入路径真的接到了窗口管理器，而不是另开一个假焦点。
- **非阻塞轮询**：`O_NONBLOCK` + `poll`，合成器主循环不被输入阻塞；没有键盘设备时启动日志明确报「no evdev keyboard」并继续运行（不崩、不假装有键盘）。

### 文件清单（P1-2a）

| 文件 | 职责 |
|---|---|
| `ci/boot-smoke.py` | `start_qemu` 增加 `-device virtio-keyboard-pci`；新增 `run_keyboard_checks()`：K1 断言合成器日志出现 `evdev keyboard attached`（EVIOCGBIT 识别取证）；K2 经 QMP `send-key tab` 注入真实按键，断言 `focused` 前后不同 |
| `aether-compositor.c` | 重写 `open_evdev()`：`openat` + `EVIOCGBIT(0)`/`EVIOCGBIT(EV_KEY)`/`EVIOCGNAME` 三重识别；`g_kbd_name[]` 记录设备名；启动日志 `aethercomp: evdev keyboard attached: %s` |
| 内核 `aetheros-kernel-6.6.70-aarch64.Image` | `CONFIG_VIRTIO_INPUT=m→y`（6.6.70 源码树交叉编译，与 `CONFIG_OVERLAY_FS` 同批） |

### 实跑验证（真 QEMU）

`boot-smoke.py --keyboard` 新增 K1/K2 两条断言（见 §十五 CI 汇总）。

### 踩过的坑（已修，如实记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `-device virtio-keyboard-device` 后客机无任何 input 设备（只有平台 gpio-keys） | 该设备挂 virtio-mmio 总线；`-M virt` 的 DT 是静态生成，mmio 传输的 virtio 设备拿不到 DT 节点，内核侧完全不可见 | 改用 PCI 变体 `virtio-keyboard-pci`（实测客机日志 `input: QEMU Virtio Keyboard as .../virtio1/input/input0`） |
| 2 | 初版 QMP chardev 用 `server=nowait` | QEMU 8.2 起该取值被废弃，只接受 `server=on/off`，QEMU 直接拒绝启动 | 改为 `server=on,wait=off`（语义不变：监听但不等待连接） |
| 3 | QMP 握手挂死（K2 超时、冒烟整体卡住 25 分钟） | 握手循环读到欢迎消息后 `continue` 继续阻塞读——欢迎消息是唯一一条非请求消息，之后再无消息可读，`readline` 无限阻塞 | 重写握手：读欢迎消息 → 发 `qmp_capabilities` → 等 `return`；并给 socket 加 30s 超时兜底 |

### 诚实边界

- **只有一个键盘设备、无触屏/鼠标**：本轮只接 virtio-keyboard（键盘是 Tab 焦点切换的充分设备）；virtio-mouse/tablet 未接，合成器的指针输入路径不存在（焦点切换靠 Tab 键而非光标点击）。
- **按键子集有限**：evdev 循环目前只对 `KEY_TAB`（焦点切换）与 `KEY_ENTER`（识别用）有意义；其他按键读进来但不改变 WM 状态——这是窗口系统输入路径的最小闭环，不是通用输入法/快捷键框架。
- **QMP send-key 是测试注入手段，不是产品路径**：真实用户的按键走 virtio 队列；K2 用 QMP 注入是因为宿主没有物理键盘中继。注入的键与真实键在客机侧走完全相同的 evdev 路径，断言焦点变化即为真实路径取证。

## 十一、P1-2b 安全沙箱：应用以非 root + 零能力 + mnt ns + seccomp 运行（2026-10-18）

「应用运行时」如果没有安全模型，就只是一堆同权限进程。本轮给服务监管加**沙箱基元**：`aether-init` 在 spawn 带 `sandbox yes` 标记的服务时，子进程 exec 前依次完成——私有 mount 命名空间、身份降为专用非 root 用户（uid/gid 1000）、清空全部能力、`NoNewPrivs` + seccomp BPF 允许列表。demo 服务是第一个沙箱化应用，`/proc/<pid>/status` 的四个字段（Uid/CapEff/Seccomp/NoNewPrivs）是它的实时取证。

> 零依赖实现：不引 libcap / libseccomp，全部走裸 `syscall(2)` 与手写 BPF 程序。合成器 socket 同步放权（`fchown(fd,-1,1000)` + mode 0660），沙箱后的 demo（gid 1000）仍能连上 IPC——降权不破坏功能，否则沙箱就是假的。

### 架构：沙箱执行序与每步的内核语义

```
aether-init spawn_service() 子进程:
  1) unshare(CLONE_NEWNS) + mount("/", MS_REC|MS_PRIVATE)
        → 服务后续挂载不影响宿主视图（需 CAP_SYS_ADMIN，此刻仍持有）
  2) setresgid/setresuid(1000,1000,1000)
        → 非 root 身份。必须在丢能力之前：setresuid(2) 规则是
          「无 CAP_SETUID 时只能把 uid 设为当前 ruid/euid/suid 之一」，
          uid 0 并不例外（CI 实测 EPERM 取证，见踩坑 #1）
  3) PR_CAPBSET_DROP(全部) + capset(0)
        → bounding/effective/permitted/inheritable 全空。
          降低自身能力集无需特权（仅提升需 CAP_SETPCAP）
  4) PR_SET_NO_NEW_PRIVS=1 + prctl(PR_SET_SECCOMP, &prog)
        → BPF 允许列表。no_new_privs 同时堵死 exec setuid 逃逸；
          seccomp 装在 setresuid 之后，不会 KILL 掉身份切换本身
```

seccomp BPF 策略（arm64，AUDIT_ARCH_AARCH64 校验后）：
- **默认动作 `SECCOMP_RET_EPERM`**：未知系统调用清晰失败（返回 EPERM 并留下 errno），而不是静默杀进程——良性但未登记的系统调用（如新 glibc 引入的）不会把应用莫名搞死。
- **显式 KILL 名单**（`SECCOMP_RET_KILL_PROCESS`）：`mount`/`umount2`/`unshare`/`setns`/`chroot`/`pivot_root`/`bpf`/`kexec_load`/`ptrace`/`reboot`/`swapon/swapoff`/`quotactl`/`settimeofday`/`clock_settime`/`setresuid/setresgid/setreuid/setregid`/`capset`/`acct`/`init_module` 等——提权与系统级操作用完即杀。
- **允许名单**按 arm64 实际存在形态登记（`openat`/`newfstatat`/`ppoll`/`pselect6` 等 *at 变体；**不登记** arm64 上根本不存在的旧调用号 `open`/`stat`/`poll`/`select`/`fork`/`dup2`/`getdents`/`sigreturn` 等）。
- **`prctl` 放行**：glibc/busybox 内部可能调用，且 filter 装好后它已无提权风险。
- **用户命名空间兜底**：`aether-init` 全局写 `/proc/sys/user/max_user_namespaces=0`——`clone3(CLONE_NEWUSER)` 的标志在指针之后，seccomp 无法检视，只能从源头禁掉 user ns。

### 文件清单（P1-2b）

| 文件 | 职责 |
|---|---|
| `aether-sandbox.h`（新建） | 契约：`AETHER_SANDBOX_UID/GID=1000`、`int aether_apply_sandbox(const char *svcname)` |
| `aether-sandbox.c`（新建，~330 行） | `drop_capabilities()`（裸 `struct __user_cap_header_struct`/`_data_struct` + `PR_CAPBSET_DROP` 循环）、`install_seccomp()`（AUDIT_ARCH_AARCH64 + 手写 BPF allow/KILL 名单，default EPERM）、`aether_apply_sandbox()`（上述执行序，每步成败上串口日志） |
| `aether-init.c` | `struct service` 加 `int sandbox`；解析 `sandbox yes\|no`；`spawn_service()` 子进程 exec 前调用；main 早期写 `/proc/sys/user/max_user_namespaces=0` |
| `make_rootfs.sh` | passwd/group 加 `aether:x:1000:1000`；demo 服务加 `sandbox yes` |
| `aether-compositor.c` | `setup_socket()` 加 `chown(SOCK_PATH,-1,1000)`——非 root demo 保持 IPC 连通（**必须用路径 chown，见踩坑 #5**） |
| `ci/boot-smoke.py` | 新增 `run_sandbox_checks()`：S0 取 demo pid；S1-S4 读 `/proc/<pid>/status` 断言 Uid 全 1000 / CapEff=0 / Seccomp=2 / NoNewPrivs=1 |

### 实跑验证（真 QEMU）

`boot-smoke.py` 默认链路新增 S0-S4 断言（见 §十五 CI 汇总）。

### 踩过的坑（已修，如实记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | S1 失败：demo 的 `Uid: 0 0 0 0`，日志 `setresuid/gid FAILED (Operation not permitted)`，而 CapEff=0/Seccomp=2/NoNewPrivs=1 全过 | 执行序错误：先 `PR_CAPBSET_DROP`+`capset(0)` 丢光能力，再 `setresuid(1000)`——无 CAP_SETUID 时 setresuid 只许设为当前 ruid/euid/suid 之一，「uid 0 总能改」是错误直觉 | 调序：**先 setresuid（仍持 CAP_SETUID）再丢能力**（降自身能力集无需特权）；注释里记录 setresuid(2) 真实规则与 CI 取证 |
| 2 | arm64 上 14 个 `__NR_*` 未声明 | arm64 无旧系统调用号（只有 `openat`/`newfstatat`/`ppoll`/`pselect6` 等 *at/变体），照 x86_64 名单照抄即编译错误 | 用 `/tmp/arm64_nr.txt`（内核 `unistd64.h` + asm-generic）提取真实可用集逐项比对后重建名单 |
| 3 | seccomp 若装在 setresuid 之前会把身份切换 KILL 掉 | BPF 一旦生效，`setresuid` 调用受 filter 管辖 | filter 必须在身份切换之后安装（本实现的序天然正确，此处固化进注释防回归） |
| 4 | `struct __user_cap_header` 未声明 | UAPI 真实类型名是 `__user_cap_header_struct`/`__user_cap_data_struct` | 改用 UAPI 原名（交叉编译器 sysroot 的 `linux/capability.h` 查证） |
| 5 | S1-S4 全 PASS 但 demo 每 10 秒重启循环：`demo: create failed`，J12/J14/J15/K2 连带失败；时间差 1.98s→12.24s 恰为 40×250ms connect 重试耗尽 | 合成器 `fchown(fd,-1,1000)` **返回 0 但静默无效**——sockfs inode 无 `setattr` 操作，fchown 对 unix socket fd 改不了 owner，socket 实际仍是 `root:root 0660`；沙箱 demo（uid/gid=1000、caps 已清、无 CAP_DAC_OVERRIDE）connect 得 EACCES。宿主最小复现实锤（fchown 后 `ls -la` 仍 root root）。宿主实验里 child 从 root setresuid 后仍能连，是因 SECURE_NOROOT 默认关闭、setresuid 保留 capabilities 掩盖了问题——客机里 capset(0) 之后才暴露 | 改 `chown(SOCK_PATH,-1,1000)`（路径 chown 走 inode setattr 正常路径）；README 记录该内核语义防回归 |
| 6 | busybox 在沙箱 sh 里 SIGSYS 重启循环（audit: `comm="sh" syscall=144 setuid code=0x80000000`） | busybox `check_suid()` 在 `ruid != 0` 时对 BB_SUID_DROP 类 applet 调 `setuid(ruid)` 降权，被 seccomp KILL 名单（setuid 系）击杀；ruid==0 时 early return 故此前 root 路径正常 | rootfs 不含任何 suid applet，关闭 `CONFIG_FEATURE_SUID/SUID_CONFIG/SUID_CONFIG_QUIET` 重建 busybox（`check_suid` 编译为 no-op），seccomp 名单保持严格不动 |

### 诚实边界

- **沙箱 ≠ 隔离一切**：mnt ns 只隔离挂载视图，不隔离 PID/网络/用户 ns（未 `unshare` 这些——应用间仍共享网络与 PID 视图）；seccomp 是允许列表兜底，不是形式化验证的攻击面模型。
- **KILL 名单是保守集，不是完备集**：目标是「提权与系统级操作用完即杀」，不是「穷尽一切危险调用」；例如 `process_vm_readv`/`perf_event_open` 一类不在名单（走 default EPERM，失败但存活）。
- **单应用验证**：目前只有 demo 带 `sandbox yes`；aetherosd/合成器/getty 仍是全权限（它们要管 socket、DRM、tty）。把沙箱推广到全部服务是后续工作，不是本轮已交付能力。
- **uid 1000 是硬编码约定**：`aether` 用户由 make_rootfs.sh 写入 passwd；换 uid 需同时改 `aether-sandbox.h` 与 rootfs 账户，没有做成可配置。

## 十二、P1-2c OTA A/B 双槽 + recovery：槽位翻转后真的启动另一个系统（2026-10-18）

「OTA」是移动操作系统的基本盘：升级不能变砖、升级失败要能回滚。本轮交付**真实的 A/B 双槽机制**——三个 virtio 磁盘（`/dev/vda`=A 槽、`/dev/vdb`=B 槽、`/dev/vdc`=持久分区），initramfs 里的启动选择器按 `/persist/slot` 标记决定本次挂哪个盘，`aether-ota` 负责翻转标记。**没有模拟、没有假重启**：切槽后 QEMU 真实重启，客机从另一块磁盘引导，`/proc/mounts` 显示根文件系统真的换了设备。

> 防砖保证：`aether-ota --switch` 在**提交翻转之前**先挂载目标槽校验 `/sbin/init`（`validate_slot()`），目标槽不可挂载/无 init 就拒绝翻转、保持原槽——CI 用 QMP 热摘 B 盘实测这条拒绝路径（阶段 B）。

### 架构：三个组件与一次切槽的完整时序

```
镜像组（ci/make_ab.sh 构建）:
  slotA.ext4 / slotB.ext4   同一系统镜像的两个副本，/etc/aether-slot 分别为 A/B
  persist.ext4              持久分区，初始内容 slot=A（恢复标志运行时写入）
  aetheros-ab-initramfs.cpio.gz   busybox + aarch64 libs + aether-ota + /init 选择器

启动（每次重启都走这里）:
  内核 -> initramfs /init（ci/ab-initramfs-init.sh）
    ├─ mount proc/sys/devtmpfs
    ├─ mount -t ext4 -o rw /dev/vdc /persist        # 持久分区
    ├─ 读 /persist/slot（A|B，缺失按 A）
    ├─ [recovery 标志存在] -> AETHEROS_RECOVERY_MODE：
    │     rw 挂活跃槽到 /mnt/slot（现场修复用）
    │     aether-ota --apply（校验活跃槽 + 清除恢复标志）
    │     reboot -f
    └─ [正常] mount -t ext4 -o rw /dev/vda|/dev/vdb /newroot
              exec switch_root /newroot /sbin/init  # 交棒 AetherOS 原生 init

切槽（客机内执行 aether-ota --switch）:
  validate_slot(目标)        # 挂载目标槽 + 检查 /sbin/init，失败即拒绝
  -> write /persist/slot     # fsync + sync 后提交
  -> kill(1, SIGUSR1)        # PID1=aether-init：优雅停机 -> reboot(RB_AUTOBOOT)
  -> 内核重启 -> initramfs 选择器读到新标记 -> 启动另一槽
```

要点：
- **选择器不静默回退**：persist 挂不上、槽挂不上、无 `/sbin/init`，都打印 FATAL 并落 emergency shell——静默回退 A 槽会掩盖持久分区损坏，比挂起更危险。
- **恢复模式是 initramfs 内的最小恢复环境**：不依赖任何槽的完整性也能起来（标记本身在 persist 上），起来后 `aether-ota --apply` 校验活跃槽并清标志，然后重启回正常槽。
- **两处槽位证据互证**：选择器按 `/persist/slot` 挂盘（外部证据），盘内 `/etc/aether-slot` 自证身份（内部证据）——不一致就意味着挂错盘，测试同时断言两者。
- **initramfs 复用同一 aether-ota 静态二进制**：恢复环境的 `--apply` 与客机内的 `--switch` 是同一个程序、同一套槽位契约。

### 文件清单（P1-2c）

| 文件 | 职责 |
|---|---|
| `aether-ota.c`（新建，195 行） | 槽位管理器：`--current`/`--switch`（先校验目标再翻转 + 通知 PID1 优雅重启）/`--mark-recovery`/`--apply`（校验活跃槽 + 清标志）；自行挂载 `/dev/vdc` |
| `ci/ab-initramfs-init.sh`（新建） | initramfs `/init` 启动选择器（上文架构中的完整决策树） |
| `ci/make_ab.sh`（新建） | 构建镜像组：交叉编译 aether-ota → 两个槽 rootfs（SLOT=A/B）→ persist.ext4 → initramfs |
| `ci/ab-smoke.py`（新建，~430 行） | A/B 冒烟断言链（A 首启 / B 拒坏槽 / C 切槽实启 / D 恢复 / E 优雅停机），含 QMP `device_del`/`device_add` 热插拔 |
| `make_rootfs.sh` | 新增 `SLOT=A\|B` 参数写 `/etc/aether-slot`；`AETHER_OTA` 参数装入 `/sbin/aether-ota` |
| `ci/run-ci.sh` | 新增 `cross-ota` 编译阶段、`ab-images` 构建阶段、`ab-smoke` 冒烟阶段；artifacts 断言纳入 aether-ota |

### 实跑验证（真 QEMU）

`ci/ab-smoke.py` 断言链（见 §十五 CI 汇总）。

### 诚实边界

- **A/B 两槽是同一镜像的两个副本**：没有实现「差分包下载/写入非活跃槽」——本轮交付的是**槽位机制与防砖保证**（校验、翻转、恢复、实启验证），OTA 包的差分与签名校验是后续工作。
- **持久分区是整盘 ext4，无掉电保护分级**：翻转后 `fsync`+`sync`，但没有 A/B 各自的 slot metadata 校验和（Android 的 bootloader BCB）；若 `slot` 文件写到一半损坏，选择器按 A 处理（缺失/乱码的既定缺省）。
- **恢复模式无 UI**：恢复环境是串口 shell + `aether-ota --apply`，没有图形恢复界面（那是 recovery 分区的活，不在 initramfs 选择器职责内）。
- **`--switch` 后的重启依赖 PID1 是 aether-init**：`request_reboot()` 检查 `/proc/1/comm`，非 aether-init 时只打印「reboot to apply」不代劳——客机内执行时由 init 的 SIGUSR1 语义完成优雅重启。

## 十三、P1-2d 多 plane 硬件合成：枚举真实 DRM 资源并如实上报（2026-10-18）

合成器的显示路径此前只假设「一个 primary plane」：`pick_plane()` 取第一个 `possible_crtcs` 匹配的 plane 就用，从不过问硬件到底给了几个 plane、什么类型。宣称「硬件合成」却不知道硬件合成器资源清单，与假装无异。本轮让合成器**枚举全部 plane**（primary/overlay/cursor 分类计数）并经 status IPC 上报，同时检测 GPU 3D 能力。

> 诚实原则：virtio-gpu 只暴露 **1 个 primary plane**（内核 `drm: virtio_gpu` 的 plane 创建只有 primary）。因此合成器如实上报 `overlay=0 cursor=0`——这是硬件事实，不是功能缺失的假声称；上报机制本身是真实的，硬件给多少就报多少。

### 架构：plane 枚举与 GPU 能力检测

```
aether_drm_open()
  ├─ pick_plane()  ← 本轮强化：遍历全部 plane 资源对象
  │    for each plane:
  │      drmModeGetPlane(res, pid)
  │      if (p.possible_crtcs & (1u << crtc_idx)):
  │          n_planes++
  │          switch (p.type): PRIMARY→n_primary / OVERLAY→n_overlay / CURSOR→n_cursor
  │      选第一个 primary 作为绘制目标（行为不变）
  └─ detect_gpu_caps()  ← 新增（P1-2e 共用）
       gp.param = VIRTGPU_PARAM_3D_FEATURES
       ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp)
       → gpu_3d = gp.value（内核 virtgpu_ioctl.c: value = vgdev->has_virgl_3d）

status IPC 新增字段:
  {"gpu":"virtio-gpu","cap_3d":0,"planes":{"total":1,"primary":1,"overlay":0,"cursor":0}}
```

要点：
- **枚举的是内核 DRM 资源，不是想象**：`drmModeGetPlane` + `drmModeGetPlaneResources` 走 ioctl 拿真实对象；plane 类型来自 `DRM_PLANE_TYPE_*`，计数与 `possible_crtcs` 掩码按本 CRTC 过滤。
- **`DRM_IOCTL_VIRTGPU_GETPARAM` 才是 virgl 能力的真实检测路径**：`DRM_CAP_3D` 不是内核 UAPI 宏（那是 libdrm 私有定义），`DRIVER_RENDER` 位永远置位（render 节点恒存在）——两者都不能用来判断 3D 能力，只有 `VIRTGPU_PARAM_3D_FEATURES`（param=1）返回 `has_virgl_3d`。
- **上报与消费解耦**：合成器只如实上报；是否用 overlay/cursor 做合成策略是后续工作（见诚实边界）。

### 文件清单（P1-2d）

| 文件 | 职责 |
|---|---|
| `aether-drm.h` | `aether_drm_t` 新增 `n_planes/n_primary/n_overlay/n_cursor/gpu_3d` 字段 |
| `aether-drm.c` | `pick_plane()` 增加分类计数；新增 `detect_gpu_caps()`（`DRM_IOCTL_VIRTGPU_GETPARAM`）；`aether_drm_open()` 打印 plane 清单 |
| `aether-compositor.c` | status IPC 增加 `"gpu"/"cap_3d"/"planes"{...}` 字段 |
| `ci/boot-smoke.py` | 新增 **J17**：正则解析 `planes` 字段，断言 `primary>=1 && total>=primary`，并把 `overlay/cursor=0` 记录为 virtio-gpu 的硬件事实 |

### 实跑验证（真 QEMU）

J17 实测输出（CI）：`total=1 primary=1 overlay=0 cursor=0`——与 virtio-gpu 内核驱动的 plane 创建事实一致。

### 诚实边界

- **virtio-gpu 单 plane 是宿命，不是 bug**：virtio-gpu 内核驱动只创建 primary plane；overlay/cursor=0 是真实硬件能力。要在 QEMU 里验证多 plane 合成，需要换硬件模型（如 vc4/pl111 有 overlay/cursor），不在本轮范围。
- **只枚举，未使用**：上报了 overlay/cursor 计数，但合成器仍只用 primary plane 做全屏 blit；多 plane 分工合成（如光标走 cursor plane、视频走 overlay plane）是后续工作。
- **没有跨 DRM 设备的合成**：单 CRTC、单 connector；多显示器/多 CRTC 热插拔未实现。

## 十四、P1-2e virgl GPU 加速：QEMU 开启 virgl + 客机真实能力检测（2026-10-18）

virtio-gpu 有两副面孔：无 3D 命令流时它是「 dumb buffer + KMS 页翻」的软件显示端（P0-6 走的就是这条路）；开 `virgl=on` 后它暴露 **VIRTIO_GPU_F_VIRGL** 特性，客机可经 Gallium/virgl 用户态驱动把 GL 命令流送进宿主 Mesa 渲染。本轮交付的是**能力侧**：QEMU 侧开启 virgl，合成器经专用 ioctl 真实检测 3D 能力并上报——先有能力探测，才谈得上后续的 GPU 加速渲染路径。

### 架构：virgl 能力检测路径

```
宿主 QEMU:
  -device virtio-gpu-gl-pci            # 本 QEMU（Debian 8.2 模块化）virgl 的载体
  -display egl-headless                # 宿主 GL 后端（需 DRM render node，见诚实边界）
       ↓ virtio 队列（3D 命令流 / 能力查询）
客机内核 virtio-gpu:
  has_virgl_3d = 1                     # 探测到 VIRTIO_GPU_F_VIRGL
       ↓ DRM_IOCTL_VIRTGPU_GETPARAM(param=VIRTGPU_PARAM_3D_FEATURES)
客机合成器 detect_gpu_caps():
  gpu_3d = 1 → status IPC "cap_3d":1
```

要点：
- **`VIRTGPU_PARAM_3D_FEATURES` 是唯一诚实的检测路径**（详见 §十三）：内核源码 `drivers/gpu/drm/virtio/virtgpu_ioctl.c` 里 `DRM_IOCTL_VIRTGPU_GETPARAM` 对 param=1 返回 `vgdev->has_virgl_3d`——设备真实宣告的能力，不是猜的。
- **宿主侧条件本宿主不满足（如实报告）**：沙箱宿主装 libvirglrenderer + Mesa，QEMU 模块目录含 `hw-display-virtio-gpu-gl.so`/`ui-opengl.so`，但**宿主无 `/dev/dri/renderD*` 且 mknod 被禁**（含 user namespace 内）——QEMU `ui-opengl` 模块与 llvmpipe（kms_swrast）都硬依赖 render node，`virtio-gpu-gl-pci` realize 即报 `opengl is not available`。因此本 CI 宿主的 `boot-smoke-virgl` 段**如实 SKIP**（宿主前置探测 `/dev/dri/renderD*`，不具备则记 SKIP 并附诊断，不计 FAIL、不伪造 PASS），G1/G2 的真跑验证留给具备 render node 的宿主；代码路径（检测 + 上报）本身已在 P1-2d 多 plane 同批实现并经默认冒烟覆盖。
- **合成器显示路径与 virgl 解耦**：KMS 页翻仍走 dumb buffer + atomic commit；3D 命令流是并列能力，本轮只检测不消费（见诚实边界）。

### 文件清单（P1-2e）

| 文件 | 职责 |
|---|---|
| `ci/boot-smoke.py` | `start_qemu` 新增 `--gpu-virgl`：`-device virtio-gpu-gl-pci` + `-display egl-headless`；main 入口宿主前置探测 `/dev/dri/renderD*`（不具备 → G 段如实 SKIP）；`run_virgl_checks()`：G1 断言 status `cap_3d=1`，G2 断言启动日志 `VIRTGPU_PARAM_3D_FEATURES=1` |
| `ci/run-ci.sh` | 新增 `boot-smoke-virgl` 阶段（独立跑一遍带 virgl 的冒烟） |
| `aether-drm.c` / `aether-compositor.c` | `detect_gpu_caps()` + status `cap_3d` 字段（与 P1-2d 同批实现，见 §十三） |

### 实跑验证（真 QEMU）

`boot-smoke-virgl` 阶段 G1/G2 断言（见 §十五 CI 汇总）。

### 踩过的坑（已修，如实记录）

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 最初想用 `DRM_CAP_3D` 检测 3D 能力 | `DRM_CAP_3D` 不在内核 UAPI 头里（libdrm 私有宏），编译期就不可用 | 改查内核源码确定 `DRM_IOCTL_VIRTGPU_GETPARAM` + `VIRTGPU_PARAM_3D_FEATURES`，并 `#include <drm/virtgpu_drm.h>` |
| 2 | 曾考虑用 render 节点存在性当 virgl 标志 | `DRIVER_RENDER` 位恒置位——`/dev/dri/renderD128` 永远存在，与 3D 能力无关 | 放弃该启发式，只用 GETPARAM 查询结果 |

### 诚实边界

- **只检测、未加速**：本轮交付「能力探测 + 上报」，合成器的渲染路径仍是软件 blit；没有引入 EGL/GLES/virgl 用户态驱动，没有把任何绘制搬到 GPU 命令流。宣称 GPU 加速是假的——宣称的是「硬件 3D 能力已确认存在且可上报」。
- **`egl-headless` 依赖宿主 GL 栈**：CI 环境具备（libvirglrenderer + Mesa）；换无 GL 的宿主必须能降级回 `-display none` + 无 virgl（软件路径），本轮未做自动降级。
- **单 GPU、单显示**：未测多 GPU/GPU 热插拔；virgl 版本协商细节（哪个 Gallium 特性集）未探测。

## 十五、P1 批次 CI 汇总：真实断言全链（2026-10-18）

§六 的两条工作流之外，P1-2 五个方向（evdev / 安全沙箱 / OTA A/B / 多 plane / virgl）的验收全部落在 `ci/run-ci.sh --skip-core` 一条全链里。以下数字全部取自 2026-10-18 沙箱实跑的 summary 文件，不是估计值：

| 阶段 | 断言数 | 结果 | 证据文件 |
|---|---|---|---|
| cross-init / cross-daemon / cross-compositor / cross-demo / cross-ota | 5 个编译阶段 | 全过（`-Wall`/`-Wextra` 零警告） | `ci-evidence` 日志 |
| artifacts（`check-artifacts.sh`） | 4 文件 ARM64 静态 ELF | 全过 | `file` + `readelf` 双证据 |
| ipc-smoke（`ipc-contract-smoke.sh`） | 13 项契约断言 | 全过 | 宿主真 UDS + JSON 契约 |
| busybox + rootfs | 构建阶段 | 全过 | 版本化配置确定性构建 |
| **boot-smoke** | **51 项断言** | **51 PASS / 0 FAIL / 0 SKIP** | `boot-smoke.summary.txt` |
| boot-smoke-virgl | G1/G2 共 2 项 | **2 SKIP / 0 FAIL**（宿主无 `/dev/dri/renderD*`，如实记 SKIP，见 §十四） | `boot-smoke-virgl.summary.txt` |
| ab-images + **ab-smoke** | **32 项断言** | **32 PASS / 0 FAIL** | `ab-smoke.summary.txt` |

合计 **85 项 QEMU 实跑断言（51 + 2 SKIP + 32）+ 13 项宿主 IPC 契约断言 + 4 项工件断言**，0 FAIL。SKIP 仅 virgl 两例且原因为宿主硬件限制（沙箱禁 `mknod`、无 render node），已在 §十四 与 boot-smoke.py 宿主前置探测中如实分流——不计 PASS 也不计 FAIL。

### 各断言段覆盖（boot-smoke 51 项）

| 段 | 覆盖 | 关键不变量 |
|---|---|---|
| A | 启动取证 | PID1=aether-init、6 服务 0 失败 |
| B/C | 登录与身份 | 真 shell 回显（防 tty 回显误判）、PID 1 comm |
| D/E/F | 服务拓扑 + IPC 契约 | PPID=1、schema、backend=linux-native、virtio_disk、uptime 活数据递增 |
| G | 9p hostshare | 挂载 + JSON share_mounted |
| J | 图形栈（P0-6/P1-1a/P1-1b/P1-2a/P1-2d） | DRM 节点、KMS 模式、多窗口、哈希稳定/变化、截图像素（demo 红块 1600 像素、中文行 959 像素）、evdev 键盘 + send-key 焦点、plane 清单 |
| K | 真实输入（P1-2a） | EVIOCGBIT 识别 QEMU Virtio Keyboard、Tab 焦点切换 |
| S | 安全沙箱（P1-2b） | uid=1000、CapEff=0、Seccomp=2、NoNewPrivs=1 |
| H | 崩溃自恢复 | kill -9 → init 重启 → 新 PID 与 ps 实测一致 |
| I | 优雅停机 | reboot(RB_POWER_OFF) → QEMU 自行退出 rc=0 |

### 各断言段覆盖（ab-smoke 32 项）

| 段 | 覆盖 | 关键不变量 |
|---|---|---|
| A | 选择器首启 slot A | banner `/dev/vda`、root 交棒实证 |
| B | 负向：坏槽拒绝 | device_del 摘盘 → `failed to mount` → `refusing to switch` → 标记保持 A → drive_add 恢复 → vdb 重现 |
| C | 正向：切槽实启 | `switched=B` → RB_AUTOBOOT → banner slot B → root=/dev/vdb → PID1=aether-init |
| D | 恢复模式 | mark-recovery → 恢复 banner → `validated=1 recovery_cleared=1` → 仍回 slot B（无回退） |
| E | 优雅停机 | PSCI poweroff，QEMU 自行退出 |

### 复现方式

```bash
BUILD_DIR=/tmp/aether-final bash ci/run-ci.sh --skip-core
```

产物体：`boot-smoke.summary.txt`、`boot-smoke-virgl.summary.txt`、`ab-smoke.summary.txt`（含每条断言的实测细节列）。`--skip-core` 跳过需要 Gradle 下载的 aether-core 构建（该段 169 项测试的通过记录见 §六 与 `ci-evidence.log`）。

### 诚实边界（CI 汇总）

- **virgl 的 2 SKIP 不是通过**：宿主无 render node 是硬事实，代码路径（GETPARAM 检测 + cap_3d 上报）本身随 boot-smoke 默认段编译并部分覆盖，但「真实 3D 命令流跑在 GPU 上」从未在本沙箱验证过。
- **ab-smoke 的盘是 QEMU 虚拟盘**：热插拔走的是 QEMU ACPI/PCIe 模拟 + 客机 pciehp 驱动真实响应链，不是物理盘；但这正是要验的内核-用户空间协作路径。
- **断言字面值全部取自取证日志**：如 `windows=2`、红色像素 1600、中文像素 959、`CapEff=0` 等，没有为让测试通过而放宽的期望值。

## 十六、P2-11 包管理器：.aepkg 签名格式 + ed25519 钉死发布密钥（2026-10-19）

应用不再「把二进制拷进 rootfs」，而是**签名打包 → 开机/运行时验签安装 → 沙箱运行**。全部实现在 `aether-pkg.c`（宿主/客机同源，一个二进制两种角色）与 `ci/` 断言链中，无任何桩。

### .aepkg 格式 v1

```
[header 64B][manifest][payload][ed25519 signature 64B]
```

| 区段 | 内容 |
|---|---|
| header | magic `AEPKG1\0\0`(8B) + version u32le + header_len u32le + manifest_len u64le + payload_len u64le + sig_len u32le + flags u32le + reserved 24B |
| manifest | 行式文本（`\n` 结尾）：`id=` / `name=` / `version=` / `exec=` / `file=<path> <size> <sha256hex>`（每文件一行） |
| payload | 各文件内容按 manifest 顺序拼接 |
| signature | ed25519（tweetnacl），**覆盖 header+manifest+payload 全内容区**——二进制改一字节、manifest 改一行、文件被截断，全部在解包前拒签 |

硬上限（分配/解包前全部校验，畸形包无法让我们过度承诺内存）：整包 64MiB、manifest 1MiB、256 文件、单文件 32MiB、path 禁绝对路径（`/app.py` 被 `valid_relpath` 拒绝）、id 1..48、name 1..63、version 1..32。manifest 每文件另带 SHA-256，解包时复核（纵深防御）。

### 密钥模型：一枚钉死的发布公钥

- 客机只认**一枚**公钥，编译期内嵌在 `aether-pkg-pubkey.h`（由 `keygen` 生成；提交公钥在密码学上本就无害）。
- `create` 用 `$AETHER_PKG_PRIVKEY`（64 字节 hex，可带 `#` 注释头）签名；签名密钥的身份在 `create` 时自认（tweetnacl 的 `crypto_sign_keypair` 末尾 `sk[32..63]` 就是公钥副本，`memcmp` 即知是否发布密钥）——**用外国密钥 create 会成功但明确 WARNING「每个 AetherOS 客机都将拒绝此包」**，专供负向测试。
- `verify`/`install` 一律对钉死公钥验签；签名者不对 → 拒（CI P4 用合法自洽的外国密钥包实证）。

### 命令与运行时路径

```sh
aether-pkg keygen priv.hex pub.hex                          # 宿主：生成发布密钥对
AETHER_PKG_PRIVKEY=priv.hex aether-pkg create out.aepkg \   # 宿主：打包+签名+自验
    aether.hello 'AetherOS Hello' 1.0.0 hello app=hello
aether-pkg verify pkg.aepkg                                 # 只验签，不安装
aether-pkg install pkg.aepkg                                # 验签→解包→更新索引
aether-pkg list                                             # 已安装应用（/data/aether/apps/index）
aether-pkg remove <id>                                      # 卸载
```

安装落到 `/data/aether/apps/<id>/`，索引 `/data/aether/apps/index`，原包留存 `/data/aether/packages/`。开机时 `pkginstall` oneshot 服务对随镜像发布的 `demo.aepkg` 走**真实 install 路径**（P0 实证）；运行时可从 9p share 安装（P2）。应用经 `aether-sandbox-run` 启动——与 init 服务沙箱同一函数 `aether_apply_sandbox()`（mnt ns + uid/gid=1000 + 能力全清 + NoNewPrivs + seccomp BPF 允许清单），只是调用方从 PID 1 变成应用运行时入口。

### 断言链（全部 QEMU 实跑，字面值取自取证日志）

宿主正反矩阵 9/9：T0 发布密钥 create/verify OK；T1 篡改 payload / T2 篡改 manifest / T4 截断 / T5 空文件 / T6 垃圾 → 全拒；T3 外国密钥 create rc=0+WARNING 而 verify rc=1 拒；install/list/remove 全流程（含外国包 install 拒签）。

客机 boot-smoke P 段（2026-10-19 实跑，`boot-smoke.summary.txt`）：

| 断言 | 不变量 | 实测 |
|---|---|---|
| P0 | 开机 pkginstall 真实安装 demo.aepkg | list 含 aetheros.demo |
| P1 | 留存安装副本可重新验签 | verify OK |
| P2 | 运行时从 9p share 安装 hello.aepkg | rc=0 + installed aether.hello |
| P2b | 安装后 list 双应用 | aetheros.demo + aether.hello |
| P3 | 篡改一字节 → 拒 | rc=1 + SIGNATURE VERIFICATION FAILED |
| P4 | 外国密钥包 → 拒（公钥钉死不是摆设） | rc=1 + SIGNATURE VERIFICATION FAILED |
| P5 | 安装的 hello 经沙箱运行 | pid=120；tick=True；uid=1000；CapEff=0；Seccomp=2 |

同一次实跑 boot-smoke 总计 **58 PASS / 0 FAIL / 0 SKIP**（含 J 段图形栈、S 段沙箱、H 段崩溃自恢复、I 段优雅停机）。

### 诚实边界

- **签名只证明来源与完整，不证明包内代码安全**——那是沙箱的活（§十一）。一个「签名正确的恶意应用」在模型内是可能的，缓解在 seccomp/降权，不在包格式。
- **索引是无锁纯文本**：并发安装可能竞争。系统按单写者构造（init/应用运行时串行化安装），journaling 索引是未来工作。
- **remove 不回滚应用数据**（只删应用目录）；**无版本/降级策略**（重装包括旧版本都接受）。
- **本次实锤的两个工程教训**（都留在代码注释里）：① 冒烟断言曾因 `con.wait()` 命中命令回显而假 FAIL——`echo MARKER` 的完成标记必须行级精确匹配（`wait_line()`）；② 客机 compositor 一度是**陈旧二进制**（源码 21:14 加了 `create` 命令，二进制还是 16:25 的旧构建），demo 的 IPC 拿到 `unknown command` 静默崩溃循环——教训是 `run-ci.sh` 全链从源码重建一切，手工拼装 BUILD_DIR 必须逐一核对 mtime。

---

## 十七、P2-13 可观测性三件套：aether-logd + aether-crashd + aether-watchdog（2026-10-19）

对应 Roadmap P2 第 13 项「Watchdog / Crash Manager / 序列化日志服务」。三者均为独立系统服务，由 aether-init 按 `/etc/aether/services` 启动并监管（restart=always）。

### 17.1 aether-logd —— 序列化日志服务

| 项 | 内容 |
|---|---|
| 协议 | Unix datagram socket `/run/aether-log.sock`（mode 0660），单行记录：`<ver> <uptime_s> <LEVEL> <service> <pid> <message...>` |
| 落盘 | `/var/log/aether.log`，每行 `ISO8601Z up=monotonic LEVEL svc[pid] msg` |
| 转发 | 同步写 `/dev/kmsg`（printk 级别映射：ERROR/CRITICAL→3，WARN→4，其余→6） |
| 轮转 | 超过 256KiB 自动滚动为 `aether.log.1`（只保留一代）；SIGUSR1 强制轮转（**测试钩子**，boot-smoke L6 用） |
| 客户端库 | `aether-log.h`（`aether_log()`/`aether_log_init()`）；datagram 发完即走，**永不阻塞业务** |
| 降级路径 | logd 缺席/不可达时直写 kmsg（失败也记、不丢、不卡）；降级期间每 64 条重试一次主路径（logd 晚于调用方启动时自动恢复） |
| 手工/测试客户端 | `aether-logctl <svc> <level> <msg...>` |

设计取舍：**不替代调用方已有的本地日志**（init 的 `/run/aether-init.log`、crashd 的 `/data/aether/crash/` 各自保留）；logd 是聚合视图，不是唯一真相——本地文件在 logd 挂掉时仍然完整。**无网络远程日志**（无 TCP/TLS 上报，那是宿主侧 9p/OTA 备份的活）。

### 17.2 aether-crashd —— 崩溃管理器

| 项 | 内容 |
|---|---|
| 触发源 | aether-init：服务**异常退出**（被信号终止或退出码非零）时经 `/run/aether-crash.sock`（datagram）发事件 `crash <svc> <pid> <code> <restarts> <uptime> <cmdline...>` |
| 报告 | `/data/aether/crash/<svc>-<epoch>-<pid>.txt`：终止原因（信号名映射，signal 11→SIGSEGV）/ monotonic uptime / 重启计数 / 墙钟 / cmdline / init 日志尾 15 行 / 序列化日志尾 15 行 |
| 边界 | 正常退出（code=0）不报——崩溃管理器不是退出码台账；SIGKILL（watchdog/CI 主动杀）算异常退出，会报 |

**诚实边界（写进每份报告）**：未配置 core dump（不碰 `kernel.core_pattern`，报告不是 core）；进程退出后 `/proc/<pid>` 消失，无 maps/status 快照。这是「日志级事后记录」，不是内核级崩溃分析。

### 17.3 aether-watchdog —— 软看门狗

| 项 | 内容 |
|---|---|
| 监控 | aetherosd + compositor（白名单；pid 从 init 的 `/run/aether-services.state` 快照取） |
| 心跳 | 每 5s：aetherosd connect 即推快照（读即活）；compositor 发 `status`（**无换行**——recvmsg 单条消息协议，与 demo 同款）；2s 超时即无响应 |
| 处决 | 连续 3 个周期无响应 → SIGKILL → init 按 restart=always 拉起新进程 |
| 硬件狗 | `/dev/watchdog` 存在则打开并每周期喂狗；缺席（QEMU 常见）如实记录「software watchdog only」——**不冒充硬件复位路径** |

价值证明：init 的 waitpid 只能发现**退出**的服务；对「进程活着但 IPC 僵死」（主循环 hang、accept 不响应）无能为力——watchdog 补的正是这个洞（W3：SIGSTOP 冻结 aetherosd → watchdog 处决 → init 拉新 pid，实跑 12 秒闭环）。

**诚实边界**：不监控 PID 1（init 退出 → 内核 panic，内核兜底）；不监控 oneshot 服务（本就退出）；心跳成功≠功能正确，只证明 IPC 路径活着。

### 17.4 断言链（boot-smoke L/W/C 段，QEMU 实跑）

同一次实跑总计 **76 PASS / 0 FAIL / 0 SKIP**：

| 断言 | 不变量 | 实测 |
|---|---|---|
| L1/L2 | logd socket 0660 + PPID=1 | srw-rw----；56 1 aether-logd |
| L3 | logctl → logd → /var/log/aether.log 全链 | `...Z up=35.21 INFO boot-smoke[128] P213_LOG_CHAIN_MARKER` |
| L4 | 行格式（墙钟+monotonic+LEVEL+svc[pid]） | 正则符合 |
| L5 | services.state 含被监控服务 | logd/crashd/aetherosd/compositor/watchdog 全 running |
| L6 | SIGUSR1 强制轮转 | aether.log.1 出现 |
| W1/W2 | watchdog PPID=1 + 形态如实报告 | 70 1 aether-watchdog；software watchdog only |
| W3a-d | SIGSTOP 冻结 → 处决 → init 拉新 pid | 138→150，全程 ~12s |
| C1 | crashd PPID=1 + socket | 55 1 aether-crashd + socket |
| C2/C2b | H/W 段 SIGKILL 的报告落盘 + cause=signal 9 | aetherosd-<epoch>-138.txt |
| C3/C4 | kill -SEGV demo → 报告 signal 11 + 诚实边界 | demo-<epoch>-72.txt |
| C5 | demo 被 init 重启（崩溃→恢复闭环） | 161 1 demo |

### 17.5 诚实边界与工程教训

- **软看门狗不是硬件看门狗**：内核无 `/dev/watchdog` 时整机级死锁（如 PID1 hang）由内核 panic 兜底，本守护只做服务级恢复；文档与日志都如实标注，不夸大。
- **轮转只保留一代**：不假装 logrotate；历史归档应由 OTA 备份 /data 或宿主 9p 承担。
- **崩溃报告不是 core dump**：无寄存器/无栈回溯（进程已退出、/proc 消失）；要栈回溯需 ptrace 陪跑或 kernel.core_pattern，均未做，不冒充。
- **日志聚合有降级窗口**：调用方早于 logd 启动时（如 init 自身）首批记录走 kmsg；客户端库每 64 条重试主路径， init 的完整日志仍在其本地 `/run/aether-init.log`。
- **本轮实锤的断言教训**（第三例回显误判家族）：`|| echo XXX_NO` 的回显参数同样会被 substring 断言误判为否——`has_sock`/`has_state` 一律行级匹配；`ps -o pid=,ppid=,comm=` 是三列，正则别多加字段；`tail -3` 会截掉文件头部的启动日志（形态报告在头部，改直接 grep 目标行）。

---

## 十八、P3-14 SystemUI 上 Linux：状态条 + Launcher（2026-10-19）

对应 Roadmap P3 第 14 项「SystemUI 迁移上 Linux」/ 分阶段计划 Phase 6。AetherOS 真 Linux 用户空间第一次有了**系统界面层**：不再是「合成器 + 一个 demo 窗口」，而是状态条 + Launcher + 从界面启动应用。

### 18.1 架构：SystemUI 是 compositor 的客户端，不是合成器的内置画面

```
aether-systemui (系统服务, PID1 监管, restart=always)
   ├─ 数据源（全部真实读 /proc 与系统文件）
   │    /proc/meminfo  /proc/loadavg  /proc/uptime  time()
   │    /run/aether-services.state（P2-13c 服务快照）
   │    /data/aether/apps/index（P2-11 包索引）
   ├─ 自绘：mmap memfd -> aether-render（矩形/8x16 ASCII/16x16 CJK）
   └─ 上屏：UDS create -> SCM_RIGHTS 共享像素 -> commit（与 demo 同协议）
```

窗口内容区 456x232：上部 36px 状态条（时钟 + 内存% + 负载），下部 Launcher 应用网格（数字键角标 + 应用名 + id）+ 服务/uptime 概览。

### 18.2 两条启动链路（都是真的）

| 链路 | 路径 | 取证 |
|---|---|---|
| 键盘 | QMP send-key → virtio-keyboard → evdev → SystemUI 读键 → fork/exec `aether-sandbox-run <app>` | 日志 `systemui: digit key 2 -> launch aether.hello` + `/proc/<pid>` comm=hello、Uid=1000 |
| IPC | `aether-sysuictl -c "launch <id\|N>"` → 同一 fork/exec 路径 | 响应 `{"ok":true,"id":"aether.hello","pid":141}` |

启动的应用经沙箱启动器降权（uid/gid=1000、能力全清、mnt ns、seccomp BPF），与 init 服务沙箱同一个 `aether_apply_sandbox()`。SystemUI 以系统服务（root）运行，因为**它需要 fork/exec 降权启动器**——这是它在服务清单里不使用 `sandbox` 指令的唯一原因。

evdev 非独占：compositor 以 `O_RDONLY` 打开键盘且未 `EVIOCGRAB`，SystemUI 可独立读同一设备（与 P1-2a 同一真输入路径）。

### 18.3 与既有断言的共处（两处解耦，一处确定化）

SystemUI 引入后有三处真实冲突，都按「不改断言强度、只去掉耦合」处理：

1. **窗口基数不是常量**：J12 从「close 后 windows == 2」改为「close 使 windows 恰减 1」（更严格，且不受功能演进影响）。
2. **窗口 id 不是常量**：K2 原本就是相对断言（`focused` 前后不同），无需改。
3. **帧哈希需要一个确定化输入**：状态条会定时重绘，J9「空闲帧哈希稳定」需要一个稳定输入——J 段先 `sysuictl -c "refresh off"` 冻结刷新，测完 `refresh on`。这是确定化测试输入（像测实时系统前先停掉干扰源），不是放宽断言：冻结期间哈希必须严格一致（实测 `5ba95c7fbf8c1b92` vs 自身），恢复后另有 U8b 断言证明 SystemUI 读数的确在推进。

### 18.4 断言链（boot-smoke U 段，QEMU 实跑）

同一次实跑 **89 PASS / 0 FAIL / 0 SKIP**（U 段 12 条）：

| 断言 | 不变量 | 实测 |
|---|---|---|
| U1 | systemui PPID=1 | 74 1 aether-systemui |
| U2 | 窗口在 compositor 窗口表 | `[{"id":2,"title":"bootdiag"},{"id":102,"title":"SystemUI"},{"id":103,"title":"demo"}]` |
| U3a | `aetheros.sysui/1` 契约 | window_id=102, clock=20:03, mem=90%, load=0.00, svc=8/11 |
| U3b | 状态条内存读数 = 客机 /proc/meminfo 实测（±2%） | sysui=90% vs guest=90% |
| U4 | 应用网格来自真实包索引 | apps=['aetheros.demo', 'aether.hello'] |
| U5a/b | IPC 启动 + 沙箱降权 | pid=137；comm=hello；uid1000=True |
| U6a/b | 数字键（真 evdev）启动 + 沙箱降权 | pid=141；comm=hello；uid1000=True |
| U7 | 截图像素实证 | 青色特征像素 3889（阈值 500） |
| U8/U8b | refresh 双向开关 + 读数推进 | off/on 均生效；uptime 36.3 → 48.9s |

### 18.5 诚实边界与工程教训

- **窗口仍带 WM chrome**：当前窗口管理器只有一种窗口类型（无 layer-shell、无无边框系统层），SystemUI 不是"系统层"，只是普通客户端窗口——不假装是专属 layer。
- **Launcher 是文本网格**：无图标渲染（无 PNG/矢量解码器），无触摸/鼠标路由（尚无 pointer 输入栈）。
- **时钟只到分钟**：秒级刷新会持续改变帧哈希，与 P0-6 的空闲帧稳定不变量冲突；需要动态验证时走 `refresh on`。
- **本轮实锤的两个坑**（都已写进代码注释）：
  1. **`create <w> <h>` 是内容尺寸，不是窗口总尺寸**。SystemUI 曾按总尺寸传 460x260，compositor 于是按 (460+4)x(260+28) 计算内容区并 mmap 478400 字节，而 memfd 只有 456x232x4=423168 字节 → 访问越界页触发 **SIGBUS，合成器当场崩溃**，客户端窗口全部丢失（J14/J15/U2 连锁假 FAIL）。
  2. **合成器重启后窗口 id 失效**：SystemUI 必须自愈（`ensure_window()`：commit 被拒 → 重新 create+buf+commit），否则出现"进程活着、日志说已提交、屏幕什么也没有"的**假上屏**。同理 `cmd_buf` 必须校验响应 `ok:true`——`sendmsg` 成功不等于被接受。
  3. **串口日志会被应用输出打断**：`ps` 的行可能被切成两半，取 pid 应走启动响应或 `/proc/<pid>`，不要解析易被打断的表格文本。

---

## 十九、手机语义整型：锁屏开机 + 全屏系统层 + 真应用 + 玻璃上游参数沉底（2026-10-20，phone-v5）

针对 phone-v4 实机反馈「不像操作系统、没有应用、还是鼠标、开始不应该是锁屏、液态玻璃看不出来」的整改。对应发布：`phone-v5`（AetherOS-Phone releases）。

### 19.1 开机语义：锁屏 → 解锁 → 启动器

`aether-systemui.c` 整文件重写：小窗 + WM chrome → **全屏系统层窗口**（`create SystemUI W H fs sys`，W/H 启动时向 compositor `status` 采样一次 KMS 模式，失败即退出由 init 重启——不猜尺寸）。开机第一屏是**锁屏**：TKEY 铺底 + 玻璃面板，大时钟（scale 4）+ 中文日期 + 电量 + 锁图标（锁梁/锁体/钥匙孔）+ 点按解锁提示；点按任意处解锁进启动器（BAR_H=48/ROW_H=72 手指尺寸行高，36x32 青色角标 + 应用名 + id）。锁屏态 `launch` IPC 与数字键一律拒绝（手机语义：先解锁）；`unlock`/`lock` IPC 双向。开机不再播种调试窗口、demo 退出开机服务链（服务清单 11 项，`pkginstall` oneshot 装全部 .aepkg）。

### 19.2 液态玻璃：上游已调好的参数原样沉进底层

`aether-render.c` 直接融合 **Kyant0/AndroidLiquidGlass（Apple Backdrop 2.0.0）**的既有配方，不重新调参：

| 层 | 配方（verbatim 取自上流派方） |
|---|---|
| 普通窗口 | `vibrancy(); blur(4.dp); lens(16.dp, 32.dp)` |
| 系统面板 | `vibrancy(); blur(4.dp); lens(24.dp, 48.dp, depthEffect=true)` |

dp→px 按 density 2.0 折算（`AETHER_GLASS_DENSITY`）；`circleMap(x)=1-sqrt(1-x²)` 折射、色差（`dispersionIntensity = chromaticAberration * ((x*y)/(w*h))`，**仅交互时开启**）、45° White@50% additive 高光、InnerShadow(8dp)、vibrancy（饱和度 1.5x）全套。TCG 适配：深内部无平方根快路、折射只在离边缘 H px 带内、模糊 4 倍降采样 box blur、色差 3 带近似仅 press>0。宿主视觉验证（x86 复刻真实合成流程导出 PPM）：玻璃透出、CJK 正确、J16 近白像素 700/791（阈值 400）、U7 青色 6832/9552（阈值 500）。

### 19.3 真应用：aetheros.settings + aetheros.clock

两个新应用（`aether-settings.c` / `aether-clock.c`，普通客户端窗口，与 SystemUI 同一软件光栅化 API）：设置 = 内核/主机名/uptime/内存/负载/CPU/服务数/电量/根卷/显示器 10 行真实读数（2s 轮询）；时钟 = HH:MM 大时钟 + 中文日期 + 真实秒进度条（200ms 轮询、秒位变化才重绘）。与 demo 一起签名打包成 .aepkg 进 `/data/aether/packages/`，开机经 pkginstall 安装（U4：apps=['aetheros.clock','aetheros.demo','aetheros.settings','aether.hello']）。

### 19.4 看门狗冻结盲区修复（本轮 CI 实跑暴露）

boot-smoke 首次全量实跑挂 W3b/W3c：H 段 kill -9 重启 aetherosd 后**数秒**即被 W3a SIGSTOP 冻结，而 watchdog 对该新 pid 尚未完成首次成功心跳——120s 启动宽限把「僵死」当「慢启动」白放，90s 测试窗口内不见处决。修复（`aether-watchdog.c`）：新增 `/proc/<pid>/stat` 调度状态探测，**宽限期内唯一例外是进程被冻结（state T/t）**——冻住的进程一步都不走，不可能「正在启动」，照计 3-miss 处决。 compositor 冷启动「先 bind 后 DRM」期间 connected-but-silent 的状态是 R/D 而非 T，不会误判（phone-v4 的误杀修复不回归）。CI 实证：冻结约 30s 后 `watchdog: killing aetherosd` → init 拉起新 pid（194→206）。

### 19.5 断言链与发布

同一次实跑 **92 PASS / 0 FAIL / 0 SKIP**（boot-smoke 91 项 + virgl 早退 + ab-smoke）。新增/重写要点：T2（先解锁 → 关掉全部非 SystemUI 窗口 → fs 坐标直算注入点击）、J13（改经 SystemUI 启动，断言 PPid==systemui pid）、J15（demo 启动后二次截图扫红）、J16/U7（中文点阵与青色特征像素上屏）、U6（virgl 无键盘时如实 SKIP）、C3/C5（崩溃测试换叶子服务 halandroid）。发布 phone-v5（tarball 21.9MB + boot-latest.sh + install-termux.sh + install.sh + README），main 分支同步。顺带修复 install-termux.sh 写死 `REL="phone-v3"`（一键安装下载旧镜像）的遗留问题：改为三级探测最新 release，兜底 phone-v5；raw.githubusercontent.com（国内常被 reset）全部换 github.com 域名。

---

## 二十、手机形态整型：竖屏 720x1600 + 照片壁纸 + 应用网格（2026-10-20，phone-v6）

针对实机反馈「还是像窗口不像手机 / 玻璃看不出来 / Termux 占半屏」的整改。谱系定位与
逐系统借鉴清单见 `AETHEROS-OS-LINEAGE.md`（AOSP / LineageOS / GrapheneOS / postmarketOS /
Ubuntu Touch / Mobian / Plasma Mobile / Sailfish / OpenHarmony 逐项对照：抄什么、抄到哪、
哪些不抄及原因）。

### 20.1 竖屏手机形态

`phone-boot.sh` 默认 `-device virtio-gpu-pci,xres=720,yres=1600`（1080x2400 手机的原生
9:20），`AETHER_RES=` 可换。SystemUI 的布局全部改为**运行期由 KMS 模式推导**（`layout_init`）：

| 项 | 横屏 1280x800（CI/桌面基线） | 竖屏 720x1600（手机形态） |
|---|---|---|
| 状态条 | 48 | 96 |
| 应用网格 | 5 列 × 160px 图标块 | 3 列 × 160px 图标块 |
| 锁屏时钟 | 4x | 6x |
| 应用名/日期/提示 | 2x | 3x |

横屏分支的数值与 v5 之前**逐像素一致**（J16/T2 的扫描带仍落在原地），因此双形态共用同一套
断言，不是放宽：`J16` 扫描带 = `y ∈ [H-48, H-32)`、`T2` 点击目标 = 首个图标块中心
（`cols = max(3, W//240)`、`tile = min(160, (W-128-(cols-1)*32)//cols)`、`margin` 居中），
`U7` 精确匹配状态条 2px 分隔线（先关闭遮挡窗口，要求 ≥ 1.8×整宽——照片壁纸铺底后，
全图数「青色像素」会被壁纸颜色污染，改精确色匹配）。

`ci/run-ci.sh` 新增 `boot-smoke-portrait` 阶段：同一 rootfs、同一断言，只把分辨率换成
720x1600。**它当场抓出两个真 bug**（见 20.2、20.3）。

### 20.2 玻璃面板像素上界（竖屏冒烟抓到的静默降级）

`GLASS_MAXPX` 原为 `1280*800`：竖屏 720x1600 = 1.152M 像素超限，`glass_panel()` 走
「too big: flat tint」分支，把整屏刷成面板染色——照片壁纸、模糊、折射全看不见，而日志
**没有任何报错**。放宽到 `1280*1600`（`glass_snap` 8MB，客机 512MB 无压力）。

> 教训：这种「超过预算就悄悄降级」的写法会静默吃掉整条视觉链路。降级本身保留（真超大
> 分辨率下的安全兜底），但必须配一条在上限处运行的测试——否则永远看不见它。

### 20.3 照片壁纸（玻璃终于有内容可扭曲）

`aether-render` 新增 P6 PPM 位图壁纸：`aether_render_load_wallpaper()` + `wall_blit_cover()`
（cover 等比铺满 + 居中裁剪，不拉伸）。合成器启动时加载 `/usr/share/aether/wallpaper.ppm`
（720x1600，`make_rootfs.sh` 从 `assets/` 拷入；来源与安卓壳同源 1440x2160 实拍），
失败则如实回退内置渐变并在日志里说明——不假装加载成功。

底部标识行改到 `fb->h - 48`（横屏仍是 752，与旧断言一致）。

### 20.4 主屏应用网格

启动器从「文字行列表」改成手机主屏的**圆角图标块网格**：`fill_round_rect()` 画圆角块
（逐行圆角内缩 + `sqrtf`）、块底部压暗一档出体积感、块内是 id 末段首字母的大字标识
（`aetheros.settings` → `S`）、块下方是应用名（超宽截断）。点击命中与绘制共用
`grid_init()` 的同一份几何，不会走偏。

### 20.5 启动体验

`boot-latest.sh` 默认**后台**跑 QEMU（`nohup ... > aetheros-boot.log`），Termux 不再占半屏，
AVNC 一开就是一整部手机；`AETHER_LOGS=1` 前缀恢复前台滚串口日志。停止用
`pkill qemu-system-aarch64`（`ps -A | grep` 判定，不依赖 Termux 是否装 procps）。

### 20.6 全屏表面的「折射边框」（实机截图里那圈紫框的成因）

`aether_render_glass_sheet()` 一直按上游 `GlassMaterials.BottomSheet` 出图：边缘 48px 是
lens 折射带 + 高光 + 内阴影 + 外阴影。这套材质是为**悬浮面板**（底部弹层/对话框）设计的——
面板有边、边外是桌面，所以边缘要"玻璃感"。

但 v5 起 SystemUI 是**铺满整屏**的系统层窗口：屏幕上根本看不到玻璃的边。硬套 BottomSheet
的结果是四周画出一圈 ~48px 的折射/高光边框，看起来像给整块屏套了个紫色相框
（用户截图里最显眼的那个"怪相"）。

修法（材质按覆盖范围选，不是按窗口类型选）：`aether_render_window()` 检测窗口是否铺满
整块 fb，铺满则走新增的 `aether_render_glass_full()`——只有 backdrop blur + vibrancy +
染色（opacity 0.42），无 lens / 高光 / 内阴影 / 外阴影。非全屏的系统面板仍走 BottomSheet
（弹层场景下边缘折射是对的）。

> 顺带解释了"液态玻璃看不出来"的另一半原因：v5 的壁纸是纯渐变，玻璃后面没有任何细节可
> 扭曲；现在换成实拍照片，模糊/折射/色差才真正可见。
