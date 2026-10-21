# AetherOS 手机版 — 在你的安卓手机上运行 (Termux + QEMU)

无需 root、不解锁 bootloader、不动系统分区。在你的安卓手机上,
以 QEMU 软件模拟运行完整的 AetherOS (真实 ARM64 Linux 6.6.70 内核 +
独立用户态: init / 合成器 / SystemUI / 日志 / 崩溃 / 看门狗)。

**它是一部手机，不是一个窗口**：默认 **竖屏 720×1600**（1080×2400 手机
的原生 9:20 比例），VNC 里铺满整块屏幕；开机先是**锁屏**（大时钟 + 中文
日期 + 电量 + 锁图标），点按解锁进启动器；桌面是**实拍照片壁纸**，界面
用 **iOS 风格液态玻璃**（背景实时模糊、圆角边缘折射、镜面高光与染色）
——由合成器以 CPU 软件渲染实现 (QEMU 客户机无真 GPU, 算法与 GPU
shader 一一对应，参数直接取自 Kyant0/AndroidLiquidGlass 上游配方)。

## 独立 App (推荐): 终端里装、App 里看

不想装 VNC 查看器、不想在 Termux 和查看器之间来回切? 用这条:

```sh
curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh" | sh
```

它做的全部事情: 下载 App 包 (桥接服务 + 前端) → 后台启动本地服务 →
自动打开 `http://127.0.0.1:8080`。你看到的使用流程:

1. **先是终端界面**: 黑底绿字, 实时滚动日志, 一个 `aether@phone:~$` 命令行
2. 点「**运行**」: 自动下载最新系统镜像 (多镜像兜底, 国内可装) 并启动 QEMU,
   终端里实时滚动安装进度和内核串口日志
3. 装完**自动切到系统画面**: 系统直接显示在这个 App 里 (内嵌 VNC 显示器),
   **点按屏幕解锁**, 点按启动器应用行启动应用——跟在真手机上一样

- 浏览器打开后「**添加到主屏幕**」= 独立 App (PWA, standalone/竖屏)
- 也可以装 `aetheros-phone-app.apk` ( Releases 页下载): 全屏 WebView 壳,
  自动探测 8080/8081/8082 端口; 服务没起时显示中文帮助页 (内含上面那条命令)
- 服务没起时 APK 不会白屏, 会显示「未连接」+ 一键命令, 照着跑完回来即可
- 换端口 `AETHER_PORT=8081`; 停止 `kill $(ss -ltnp | grep ':8080 ' | grep -oP 'pid=\K[0-9]')`
- 日志 `tail -f ~/aetheros-app/server.log`

下面的「传统方式」 (Termux + AVNC) 依然可用, 与 App 共用同一份系统镜像。

## 一键运行 (复制到 Termux 里执行)

```sh
pkg update && pkg install -y qemu-system-aarch64-headless
mkdir -p ~/aetheros && cd ~/aetheros
curl -L -o aetheros-phone.tar.gz <下载链接, 见对话>
tar xzf aetheros-phone.tar.gz
./phone-boot.sh
```

> 注意: 包名是 `qemu-system-aarch64-headless` (主仓库, 装出的二进制仍叫
> `qemu-system-aarch64`)。若你的源里有 `x11-repo`, 也可 `pkg install
> x11-repo` 后装 `qemu-system-aarch64`。
>
> 用 `boot-latest.sh` 时 QEMU 会**后台运行**，Termux 窗口不占屏；要看
> 内核日志就加 `AETHER_LOGS=1` 前缀（前台滚动）。

## 启动后怎么看画面

1. 手机应用商店/F-Droid 安装 **AVNC** (VNC 查看器)
2. AVNC 新建连接, 地址 `127.0.0.1`, 端口 `5900`
3. 在 AVNC 里把画面设为**填满屏幕 / 全屏**（默认竖屏，比例与手机一致，
   几乎无黑边）。先看到 **锁屏**，**点按屏幕任意处解锁**进启动器

## 操作方式

- **开机先是锁屏**（大时钟 + 日期 + 电量 + 锁图标，全屏液态玻璃面板）：
  **手指点按屏幕任意处解锁**进入启动器
- **手指点击 Launcher 里的应用行** = 启动该应用（主交互方式，手机就该这么用）
- **VNC 窗口内打字/按键** = 客机键盘 (virtio-keyboard)
- SystemUI 解锁后按 **数字键 1-9** 也可启动对应已安装应用（键盘兜底）
- 文字内核日志在 Termux 终端里直接滚动（`AETHER_LOGS=1` 前台模式）
- 退出虚拟机: 在 Termux 里按 `Ctrl-A` 再按 `X`；后台模式用 `pkill qemu-system-aarch64`

> 触摸由 QEMU `virtio-tablet-pci` 提供绝对指针，compositor 作为输入中枢把
> 点击命中 SystemUI 窗口后，把局部坐标经 `SYSUI_SOCK` 的 `tap` 命令转发，
> SystemUI 锁屏态点按即解锁、启动器态做行命中并启动应用。

## 竖屏形态与壁纸（想换就说）

默认 `720x1600`（9:20）。换分辨率用环境变量：

```sh
AETHER_RES=1080x1920 ./phone-boot.sh     # 16:9 竖屏
AETHER_RES=1280x800  ./phone-boot.sh     # 横屏（桌面基线，开发调试用）
```

SystemUI 会按屏幕比例自动切布局：竖屏 = 手机形态（状态条 96、行高 148、
锁屏时钟 6x）；横屏 = 桌面基线（状态条 48、行高 72、时钟 4x）。
壁纸是 `/usr/share/aether/wallpaper.ppm`（P6 PPM，按 cover 等比铺满，
不拉伸）；文件缺失时合成器如实回退内置渐变并在日志里说明。

## 安卓兼容层：把真机电量/网络喂给 AetherOS（可选）

`phone-boot.sh` 会自动启动 `aether-android-bridge.sh`，把 Android 宿主的
真实电量、充电状态、Wi-Fi 名称、通知数，经 QEMU 9p 共享目录喂给客户机的
`aether-hal-android` 服务，SystemUI 状态栏右侧即显示 `bat 87%` 等真实读数。

- 装了 **Termux:API**（`pkg install termux-api` + 应用商店装 Termux:API）
  → 电量/充电/Wi-Fi/通知全部真实；
- 没装 → 自动回退读 `/sys/class/power_supply` 的电量；
- 都没有 → 状态栏显示 `bat --`（诚实标记，绝不伪造数值）。

AetherOS Core 只看到一份"硬件事实快照"，不知道它来自 Android——这正是
计划书「所有 Android 能力必须经过 Backend/HAL」原则的落地。

## 低内存手机调优

手机运行内存小于 4GB 时, 用环境变量降配:

```sh
AETHER_MEM=384M AETHER_SMP=1 ./phone-boot.sh
```

## 极慢手机：放宽看门狗启动宽限期

AetherOS 的软看门狗会监控合成器等服务的「活跳」。手机上纯软件模拟
(TCG) 冷启动慢，合成器要做完 DRM 初始化才应答心跳——默认给 120s
启动宽限期，首次成功心跳后即进入正常监管。如果您的手机特别慢（冷启动
超过 2 分钟，串口日志出现 `watchdog: compositor ... heartbeat missed`
刷屏），把宽限期调大：

```sh
AETHER_APPEND="console=ttyAMA0 root=/dev/vda rw ip=dhcp aether_wdt_grace=300" ./phone-boot.sh
```

调参会打印 `aether-init: watchdog startup grace = 300s (kernel cmdline)`
确认生效。正常手机不需要动它。

## 诚实说明 (重要)

1. **这是模拟运行, 不是原生刷机**。没有 KVM/root 时 QEMU 走纯软件
   模拟 (TCG), 启动约 1-3 分钟, 操作有可感延迟。这是目前唯一不需要
   解锁 bootloader 就能在任意安卓手机上运行的方案。
2. **`-snapshot` 模式**: 客机所有写入进入临时层, 退出即丢弃。
   在 SystemUI 里安装的应用下次启动会消失。防止镜像被玩坏。
3. **通话/短信/移动网络不可用**: AetherOS 没有 Android 的 RIL 基带层,
   virtio-net 只提供 QEMU 用户态网络 (NAT), 客机可以 DHCP 拿地址,
   但没有出口路由规则保证上网。
4. 已验证环境: QEMU 8.2 (TCG), 89 项启动断言全过。手机端 QEMU 版本
   差异可能导致个别设备型号不被识别, 如遇启动失败请把 Termux 里的
   报错发回来。

## 文件清单

| 文件 | 说明 |
|---|---|
| `aetheros-kernel-aarch64.Image` | ARM64 Linux 6.6.70 内核 (~41MB) |
| `aetheros-rootfs.ext4` | 根文件系统, 实际占用 22MB (~41MB 镜像) |
| `phone-boot.sh` | 启动脚本（含 9p 共享 + 桥接拉起） |
| `aether-android-bridge.sh` | 安卓宿主 HAL 桥接（电量/网络 -> 客户机） |

## 本包发布前实测记录 (2026-10-20 · 手机形态：竖屏铺满 + 照片壁纸 + 应用网格)

- **双形态 CI 全绿**：横屏 1280×800（桌面基线）与竖屏 720×1600（手机形态）
  各跑一遍完整 boot-smoke，加 virgl 降级 + A/B 升级链；断言坐标由 KMS 模式
  推导（状态条高度、应用网格几何、中文标识行 y = H-48），不是写死坐标
- **竖屏抓到的两个真 bug**（这正是加竖屏冒烟的价值）：
  1. `GLASS_MAXPX` 卡在 1280×800：竖屏 1.152M 像素超限 → 玻璃面板**静默降级成
     纯色块**，整屏被刷成面板染色（照片壁纸完全看不见）。已放宽到 1280×1600
  2. 状态条分隔线断言被应用窗口压住：改为先关闭遮挡窗口再数整行精确色
- **手机形态细节**：默认 720×1600 竖屏（9:20）；SystemUI 按屏幕比例切布局
  （竖屏：状态条 96 / 网格 3 列 × 160px 图标块 / 锁屏时钟 6x；横屏：48 /
  5 列 / 4x）。壁纸 `wallpaper.ppm` 按 cover 等比铺满，不拉伸
- **启动体验**：QEMU 后台运行，Termux 不再占半屏；`AETHER_LOGS=1` 前台滚日志；
  停止用 `pkill qemu-system-aarch64`

- 完整 CI 全绿后重新构建 rootfs（boot-smoke **92 项断言 0 失败** + virgl + A/B 升级链）
- **开机即锁屏**：全屏系统层窗口（液态玻璃面板）绘大时钟 4x + 中文日期 +
  电量 + 锁图标，点按任意处解锁才进启动器——与真手机开机语义一致
- **液态玻璃参数原样沉到底层**：直接采用 Kyant0/AndroidLiquidGlass
  （Apple Backdrop）已调好的配方——`vibrancy() + blur(4dp) + lens(16dp,32dp)`
  （普通窗口）/ `lens(24dp,48dp, depthEffect)`（系统面板），dp→px 按
  density 2.0 折算；色差/高光/内阴影全套，色差仅交互时开启。宿主视觉验证：
  玻璃透出、CJK 中文点阵真实上屏（截图近白像素 700+，青色特征像素 9552）
- **真应用**：`aetheros.settings`（内核/主机名/uptime/内存/负载/CPU/服务数/
  电量/根卷/显示器 10 行真实读数）与 `aetheros.clock`（HH:MM 大时钟 + 中文
  日期 + 真实秒进度条）随镜像打包，开机经 pkginstall 签名安装进启动器
- **看门狗冻结盲区修复**：init 重启服务后数秒内即被 SIGSTOP 冻结时，旧逻辑
  会因「新 pid 尚未完成首次成功心跳」而白放整个 120s 启动宽限。现增加
  /proc 调度状态探测：进程被冻结（state T）不可能「正在启动」，照计 3-miss
  处决（CI 实证：冻结约 30s 后处决 -> init 拉起新 pid）
- TCG 纯软件模拟实跑: 11 个服务全部启动 0 失败 (logd/crashd/aetherosd/
  compositor/watchdog/systemui/halandroid/getty 等)
- 触摸端到端: VNC/tablet 注入点击 -> compositor 路由 -> SystemUI 锁屏解锁/
  启动器行命中 -> 沙箱四层校验 -> 应用启动（CI T2/U5/U6 断言实证）
- TCG 冷启动到锁屏可见: 约 1-2 分钟 (视手机性能)
