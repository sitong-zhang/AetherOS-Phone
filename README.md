# AetherOS-Phone

[![Release](https://img.shields.io/github/v/release/sitong-zhang/AetherOS-Phone?label=release)](https://github.com/sitong-zhang/AetherOS-Phone/releases)
[![Image size](https://img.shields.io/github/download-size/sitong-zhang/AetherOS-Phone/phone-v7/aetheros-phone.tar.gz?label=image%20size)](https://github.com/sitong-zhang/AetherOS-Phone/releases/download/phone-v7/aetheros-phone.tar.gz)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

在安卓手机上通过 **Termux + QEMU** 纯软件模拟运行完整 AetherOS：
真实 ARM64 Linux 6.6.70 内核 + 独立用户态，无需 root / 无需解锁 bootloader。

**它是一部手机，不是一个窗口**：默认**竖屏 720×1600**（1080×2400 手机的原生
9:20 比例），VNC 里铺满整块屏幕；开机先是**锁屏**（大时钟 + 中文日期 + 电量 +
锁图标），点按解锁进**应用网格主屏**；桌面是**实拍照片壁纸**，界面用
**iOS 风格液态玻璃**（背景实时模糊 + 边缘折射 + 镜面高光，参数直接取自
Kyant0/AndroidLiquidGlass 上游配方，CPU 软件渲染），并带 **安卓兼容层**：
把真机电量/充电/Wi-Fi/通知经 HAL 后端喂给系统。

## 独立 App（推荐）：终端里装、App 里看

不再需要「Termux 看串口 + 单独开 VNC 查看器」。Termux 里跑这一条，
它自动装好 App 服务并打开——**先是终端界面，点「运行」自动装系统，
系统直接在这个 App 里显示**，点按屏幕解锁、点按启动器行启动应用：

```sh
curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh" | sh
```

> 备用镜像入口（上面不通时依次换）：
> `curl -fsSL "https://ghfast.top/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh" | sh`
> ／ `curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh | sh`

- 打开 `http://127.0.0.1:8080`：**终端面板**（实时滚动安装/内核日志 + 命令行）→ 点「**运行**」→ 自动下载最新系统镜像并启动 → 自动切到**系统画面**（内嵌 VNC 显示器，可触摸点按 + 键盘）
- 浏览器里「**添加到主屏幕**」即成为独立 App；也可装 `aetheros-phone-app.apk`（全屏 WebView 壳，自动探测 8080/8081/8082 端口，服务没起时显示中文帮助页）
- 换端口：`AETHER_PORT=8081`；低内存：`AETHER_MEM=384M AETHER_SMP=1`；日志：`tail -f ~/aetheros-app/server.log`
- 停止服务：`kill $(ss -ltnp | grep ':8080 ' | grep -oP 'pid=\K[0-9]')`

## 一条命令：自动安装并运行最新版（传统方式）

Termux 里执行这一行，它会**自动探测 GitHub 最新 release、下载、解包并启动**——永远跑最新版（将来出 phone-v7/v8 也照跑）：

```sh
curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh" | sh
```

> **国内网络可直接用上面这条**：脚本内置「直连 GitHub → gh-proxy.com 反代 →
> ghfast.top 反代」多通道自动兜底，哪个通就用哪个——Termux 里直连
> `github.com:443` 常被 TCP 阻断（`curl: (28) Failed to connect` / `(35)` reset）
> 也能装。上面这条用镜像入口拉脚本，最稳。
>
> - 备用镜像入口：`curl -fsSL "https://ghfast.top/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh" | sh`
> - 直连（仅网络通畅时）：`curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh | sh`
> - curl 坏时用 wget 版（同一 URL）：`wget -qO- "<同一URL>" | sh`
> - 三个通道都连不上时，设代理再跑：`export ALL_PROXY=socks5://127.0.0.1:1080`（手机 VPN 本地端口）或 `http://127.0.0.1:7890`

- 自动探测最新版本 → 多通道下载镜像（约 20MB）→ 解压 → 前台启动
- 启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`
- 交互：**先点按屏幕任意处解锁**（开机是锁屏），再**点按启动器应用行**或数字键
  **1-9** 启动对应应用
- 退出：Ctrl-A 再按 X；低内存手机：`AETHER_MEM=384M AETHER_SMP=1` 前缀同一行

## 一行安装（手机 Termux）

```sh
curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/install-termux.sh" | sh
```

脚本会自动：装依赖 → 下载镜像（约 20MB）→ 解压 → 后台启动 → 打印连接信息。
启动后用 AVNC（VNC 查看器）连 `127.0.0.1:5900`，VNC 窗口内**先点按屏幕解锁**，
再点按应用行（或数字键 **1**）启动应用。

> 只装不启动：`sh install-termux.sh --no-run`

## 手动安装

```sh
pkg install qemu-system-aarch64-headless   # 主仓库正确包名（二进制仍叫 qemu-system-aarch64）
mkdir -p ~/aetheros && cd ~/aetheros
curl -L -o aetheros-phone.tar.gz "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/aetheros-phone.tar.gz"
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

phone-v7 起随 release 发布的 App 资产（不在镜像包内）：

| 文件 | 说明 |
|---|---|
| `phone-app.sh` | App 一键启动脚本（永远拉最新版） |
| `aether-phone-app.tar.gz` | App 包：`server.py` 桥接服务 + 前端（终端面板 + 内嵌 VNC 显示器） |
| `aetheros-phone-app.apk` | Android WebView 套壳 APK（源码在 `apk-src/`，`build-phone-app-apk.sh` 复现构建） |

## 发布说明

镜像详见 Releases 页的 `phone-v7`。

**phone-v7**（2026-10-20 · 独立 App：终端里装、App 里看）：

- **全新 App 形态**：`phone-app.sh` 一条命令拉起本地服务（纯 Python 标准库，
  零第三方依赖）——打开先是**终端面板**（SSE 实时滚动安装/内核串口日志），
  点「运行」自动下载最新系统镜像并启动 QEMU，随后自动切到**内嵌 VNC 显示器**，
  系统直接在这个 App 里显示；彻底告别「Termux 看串口 + 单独开 AVNC」的割裂流程
- **手写 websockify + 最小 RFB 3.8 客户端**：服务端手写 RFC6455 帧编解码把
  浏览器 WebSocket 桥到 QEMU VNC（127.0.0.1:5900）；前端直接解 VNC 协议
  （raw 编码、32bpp truecolor），触摸 PointerEvent + KeyEvent 全交互
- **真 · Android APK**：`aetheros-phone-app.apk` 全屏 WebView 套壳
  （竖屏锁定、沉浸式、端口自动探测 8080/8081/8082），服务未启动时显示
  中文帮助页；`build-phone-app-apk.sh` 可从 `apk-src/` 一键复现构建
- **PWA**：可「添加到主屏幕」当独立 App 用（standalone + portrait + 缓存外壳）
- **永远最新**：`releases/latest/download` + 三级镜像兜底
  （gh-proxy.com → ghfast.top → 直连），国内网络可直接装

**phone-v6**（2026-10-20 · 手机形态：竖屏铺满 + 照片壁纸 + 应用网格）：

- **竖屏 720×1600**：1080×2400 手机的原生 9:20 比例，VNC 里铺满整块屏幕——
  不再是「显示器里的小窗口」；默认后台启动 QEMU，Termux 不占半屏
  （`AETHER_LOGS=1` 才前台滚日志）；`AETHER_RES=` 可换分辨率
- **实拍照片壁纸**（山景）：玻璃的模糊/折射/色差终于有内容可扭曲。顺带修掉
  一个真实 bug——玻璃面板的像素上界卡在 1280×800，竖屏超限后静默降级成
  「纯色块」（整屏被刷成面板染色），是新增的竖屏冒烟抓到的
- **主屏改成应用网格**：圆角图标块 + 字母标识 + 名称（竖屏 3 列），
  不再是文字行列表
- **看门狗冻结盲区修复**：init 重启服务后数秒内被 SIGSTOP 冻结时，旧逻辑因
  「新 pid 尚未完成首次成功心跳」白放整个 120s 启动宽限；现按 /proc 调度
  状态识别「冻结 ≠ 慢启动」，照常 3-miss 处决
- **一键安装器修复**：`install-termux.sh` 此前写死 `phone-v3`（下载的
  一直是旧镜像），改为三级探测最新 release；raw.githubusercontent.com
  （国内常被 reset）全部换 github.com

**phone-v5**（2026-10-19 · 锁屏开机 + 真应用 + 液态玻璃底层化）：

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
