# AetherOS 的开源移动系统谱系与借鉴清单

> 目的：把「兼容并蓄」落到可执行的工程判断上——每个开源系统抄什么、抄到哪一步、
> 哪些明确不抄以及为什么。所有「已有」项在本仓库都有代码与 QEMU 实跑断言；
> 所有「不做」项写明代价，不含糊其辞。

## 零、先定性：AetherOS 属于哪一类

AetherOS = **ARM64 Linux 内核 + 完全自有的用户态**（PID 1 / 服务监管 / 合成器 /
SystemUI / 包管理 / 沙箱 / 日志·崩溃·看门狗），不含任何 AOSP 组件，也不跑 ART。

| 谱系 | 代表 | 系统服务层 | 应用运行时 | AetherOS 的关系 |
|---|---|---|---|---|
| AOSP 系 | LineageOS / GrapheneOS / CalyxOS | Android init + SystemServer | ART（APK） | **架构同构、实现独立**：Init/合成器/SystemUI/包管理/沙箱/日志全部自研对应物 |
| 原生 Linux 系 | postmarketOS / Mobian / Ubuntu Touch / Plasma Mobile / Sailfish | systemd/OpenRC + Wayland 合成器 + 触屏壳 | 原生 ELF + 可选兼容层 | **同族**：我们就是这一类的最小实现（无 Wayland，自有合成器协议） |
| 独立微内核 | OpenHarmony | Ability 框架 + 分布式软总线 | ArkTS/原生 | 只借「Ability ≈ 应用包 + 沙箱启动器」的抽象，不照搬微内核 |
| 闭源 | iOS | — | — | 不在参考范围 |

**一句话**：AetherOS 是 postmarketOS 的同族（真 Linux 手机系统），但把 AOSP 那套
「系统服务怎么切分」的工程经验整套搬了过来——这正是它看起来「像个手机系统」的原因。

## 一、逐项对照

### 1. AOSP / LineageOS / GrapheneOS / CalyxOS

| 它的关键机制 | AetherOS 对应实现 | 状态 |
|---|---|---|
| `init.rc` 声明式服务（service/oneshot/restart/depends） | `/etc/aether/services` + `aether-init.c`（未知 depends 直接禁用，依赖拓扑启动） | ✅ 已有 |
| SurfaceFlinger 图层合成 | `aether-compositor`（fs 全屏层 / sys 系统层在下 / 普通窗口按创建序） | ✅ 已有 |
| SystemUI（状态条/通知/锁屏） | `aether-systemui`（锁屏→启动器两态状态机，全屏系统层窗口） | ✅ 已有 |
| PackageManager + APK 签名 | `.aepkg` + ed25519 **全内容签名**、公钥钉死在二进制里（比 v1 APK 签名更严） | ✅ 已有 |
| zygote + SELinux 域隔离 | `aether-sandbox-run`：uid/gid 1000、能力集清零、mnt ns、seccomp BPF 允许列表、NoNewPrivs | ✅ 已有（等价于「单域 + 最小权限」，不是 SELinux 多域） |
| logd/logcat | `aether-logd`（ISO8601 墙钟 + monotonic + LEVEL + svc[pid]，SIGUSR1 轮转） | ✅ 已有 |
| tombstone 崩溃报告 | `aether-crashd`（信号/命令行/边界声明落盘 `/data/aether/crash`） | ✅ 已有 |
| init 看门狗（service 心跳） | `aether-watchdog`（IPC 心跳 + 启动宽限 + 冻结识别） | ✅ 已有 |
| A/B 无缝升级 + 回滚 | slotA/slotB/persist + initramfs 选择器 + `aether-ota` | ✅ 已有 |
| **binder IPC** | 不用 binder：`AetherBus` = Unix domain socket + 单条消息协议 | ❌ 不抄：单机系统无 vendor 生态，binder 的复杂度换不到收益 |
| **ART / APK 运行时** | 不跑 APK；应用是静态链接的原生 ELF | ❌ 不抄：需要 libhybris 级工作，且与「真 Linux 系统」的定位冲突 |
| 厂商 blob / 设备树适配 | 发布物 = 内核 Image + rootfs 两件套（设备适配在 QEMU virt 上完成） | ❌ 不抄：没有真机 BSP 生态 |
| GrapheneOS 的内存分配器硬化 / exploit mitigation | 未做（需重编内核配置 + 分配器改造） | ⏳ 计划（内核侧） |

**从 LineageOS 只学一条**：纯净、零预装。我们的镜像开机只有 4 个应用
（设置/时钟/demo/hello），没有一条广告或后台服务。

### 2. postmarketOS（借鉴价值最高）

| 它的机制 | AetherOS | 状态 |
|---|---|---|
| Alpine + OpenRC 的**声明式服务依赖**、oneshot 语义 | `aether-init` 的 `depends/oneshot/restart` 完全同构 | ✅ |
| **内核与 rootfs 解耦**（设备移植 = 换内核 + 换 rootfs） | 发布物就是 `aether-kernel` + `aether-rootfs.ext4`，可独立替换 | ✅ |
| Phosh 壳：**BOOT → LOCK → HOME → APP** 状态机、锁定态点按/上滑解锁 | `aether-systemui` 同一状态机（本版刚落地锁屏开机） | ✅ |
| 顶部状态条（时钟/电量/网络）、应用网格从 `.desktop` 生成 | 状态条真实读数（`/proc` + HAL 快照）；应用网格从 `.aepkg` 索引生成 | ✅ |
| oFono/ModemManager 电话栈 | 无基带驱动、无 RIL | ❌ 不抄（见诚实边界） |

### 3. Ubuntu Touch / Lomiri

- 借「**系统跑在容器里、界面全屏**」的体验契约：本版起 QEMU 默认后台运行，
  VNC 一开就是一整部手机（Termux 不再占半屏）。
- 它的「应用作用域 + 内容枢纽（Content Hub）」是**未来**才值得抄的：让应用只能
  经系统枢纽交换文件/文本，而不是各写各的。当前不做（收益低于成本）。

### 4. Mobian / Debian

- 借「**复用成熟用户态工具链**」：我们用 busybox 提供 sh/mount/ps/getty 等，
  系统服务全部自研——与 Mobian「Debian 底座 + Phosh」的分层思路一致。

### 5. Plasma Mobile / KDE

- 它的核心是 **Wayland + layer-shell 协议**。我们没有 Wayland，但在合成器里
  实现了同构概念：`fs`（全屏应用层）、`sys`（系统层，渲染在普通窗口**之下**、
  点击排在普通窗口**之后**）。这是私有版的 layer-shell，语义对齐、实现极简。

### 6. Sailfish OS

- **手势优先**（上滑回桌面、左右滑动 peek）是它最值得抄的部分。我们目前是
  点按式（tap 路由 + 600ms 触摸反馈圆点）；上滑手势已列入下一步。
- 它的「安卓兼容层」是 libhybris 级工作，我们不抄——我们的 `aether-hal-android`
  是**宿主数据桥**（把安卓真机的电量/Wi-Fi/通知经 9p 喂给客机 HAL），
  不是 APK 运行时，文档与界面上都如实标注。

### 7. OpenHarmony

- 架构参考：**Ability ≈ 我们的「.aepkg 包 + 沙箱启动器」**（一个可安装、可授权、
  可独立启动的能力单元）；「分布式软总线」对应我们的 AetherBus（但只做单机）。
- 不照搬微内核：把现有 Linux 内核换掉意味着重写驱动、内存管理、IPC 全部，
  与「在手机上能跑」的目标直接冲突。

## 二、本版（phone-v6）从清单里实际落地的三件事

1. **竖屏手机形态**（Phosh/Sailfish 的形态学）：默认 720×1600（1080×2400 手机的
   原生 9:20），VNC 里铺满整块手机屏；SystemUI 竖屏布局（状态条 96 / 行高 148 /
   锁屏时钟 6x），不再是「显示器上的小窗口」。
2. **真照片壁纸**（Sailfish 的 ambience / Plasma 的 wallpaper 概念）：山景照片
   铺底，玻璃的模糊/折射/色差才有内容可扭曲——**液态玻璃终于肉眼可见**。
3. **后台启动 + VNC 全屏**（Ubuntu Touch 的体验契约）：一条命令后台起 QEMU，
   Termux 不再占半屏；`AETHER_LOGS=1` 才前台滚日志。

## 三、诚实边界（不粉饰）

- **无基带/RIL**：通话、短信、移动数据不可用。postmarketOS 靠 oFono + modem 驱动，
  我们没有 modem 驱动，也没有电话栈。
- **无安卓应用兼容**：不跑 APK。需要 binder + ART + libhybris 级工程。
- **CPU 软件渲染**：无 GPU 合成（virgl 只做能力检测与如实报告）。
- **单设备**：QEMU virt 平台；真机移植需要 BSP（这正是 postmarketOS 的设备树模型，
  我们的内核/rootfs 分层已为其留好位置）。
