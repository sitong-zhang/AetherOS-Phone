# 安卓兼容层上游源码来源（SOURCE.md）

本目录 `third_party/android-compat/` 原样保存安卓兼容层「照搬」所依据的开源
上游源码与文档副本。AetherOS 客机是**纯 C 逐像素软渲染 + 精简 Linux 用户态**，
无法整段编译上游的 Python/Kotlin/Java/GLSL，故采用与液态玻璃同一套形态：
**上游原样快照 + LICENSE + SOURCE.md + MAPPING.md + SHA256SUMS + C/Java 忠实重实现**。

## 1. Waydroid（内层 LXC / AOSP 容器）

- 仓库：https://github.com/waydroid/waydroid
- 许可：**GPL-3.0**（`upstream/waydroid/LICENSE` 为全文）
- 用途：用 Linux 命名空间（user/pid/uts/net/mount/ipc）+ LXC 在普通 GNU/Linux
  上跑完整 Android 用户态，镜像为基于 LineageOS 的最小定制系统。
- 纳入文件：
  - `upstream/waydroid/lxc.py` — 容器配置与启动：`lxc.mount.entry` 挂载序列、
    `/dev/binder|vndbinder|hwbinder` bind、`devpts`、binderfs 控制节点分配、
    `lxc-start ... -- /init`、`ANDROID_ENV`（ANDROID_ROOT/ANDROID_DATA 等）。
  - `upstream/waydroid/drivers.py` — binder 设备节点探测与 binderfs 分配。
  - `upstream/waydroid/README.md`、`LICENSE`。
- 对应实现：`src/aether-android.c`（unshare + mount + chroot + exec /init）、
  `src/aether-android.h`（状态/路径/协议）；逐条见 MAPPING.md A 节。

## 2. AOSP Android Virtualization Framework / pKVM（外层）

- 仓库：https://android.googlesource.com/platform/packages/modules/Virtualization
- 许可：Apache-2.0（AOSP）
- 沙箱内 googlesource 直连超时（40s），官方 README 内容经检索抓取，作为**带来源
  标注的摘录**纳入（非 git clone）：
  - `upstream/aosp-avf/framework-virtualization-README.excerpt.md` —
    「Detecting AVF Support」官方 Java 代码（VirtualMachineManager /
    getCapabilities / CAPABILITY_PROTECTED_VM / hasSystemFeature）、
    「Starting a VM」最小示例（VirtualMachineConfig.Builder / getOrCreate / run）、
    @SystemApi 与 MANAGE_VIRTUAL_MACHINE 权限说明、API 34 起可用。
  - `upstream/aosp-avf/pkvm-detection.excerpt.md` —
    `ro.boot.hypervisor.protected_vm.supported` / `.vm.supported`、
    `/proc/bootconfig`、`/apex/com.android.virt/bin/vm`、`/dev/kvm` 检测口径，
    以及「AVF 面向 headless / Microdroid、不提供图形 HAL」的限制说明。
  - 获取日期：2026-11-09。
- 对应实现：`vm/android/java/dev/aether/vm/AvfCapability.java`、
  `AvfLauncher.java`、`AvfPayload.java`；逐条见 MAPPING.md B 节。

## 3. Linux 内核 UAPI（输入桥）

- 来源：本机 `/usr/include/linux/`（内核 UAPI 头文件）
- 许可：**GPL-2.0 WITH Linux-syscall-note**（UAPI 例外）
- 纳入文件：
  - `upstream/linux-kernel/uinput.h` — uinput_setup / UI_SET_EVBIT /
    UI_SET_ABSBIT / UI_SET_KEYBIT / UI_ABS_SETUP / UI_DEV_SETUP / UI_DEV_CREATE /
    UI_DEV_DESTROY 的 ioctl 与结构体定义。
  - `upstream/linux-kernel/input-event-codes.h` — EV_ABS / EV_KEY / EV_SYN、
    ABS_X / ABS_Y / BTN_TOUCH / SYN_REPORT 常量。
- 对应实现：`src/aether-android-bridge.c` 的 `ensure_uinput()` 与
  `aether_android_bridge_input()`；逐条见 MAPPING.md C 节。

## 4. 图形桥（共享内存 buffer）

- 依据：Waydroid `platform_hardware_wayland` 的 hwcomposer 共享内存回退路径
  （memfd_create + wl_shm，BGRA/RGBA 拷贝）与 `IWaydroidDisplay` HAL 的
  surface/buffer 元数据交换（见检索记录 deepwiki waydroid hwcomposer）。
- AetherOS 为纯 C framebuffer，不依赖 Wayland/GPU，故用 mmap 文件 + 定长 64B
  帧头（`struct aether_android_fb`）实现等价的「生产者写帧 / 消费者贴图」协议。
- 对应实现：`src/aether-android.h` 协议 + `src/aether-android-bridge.c`
  `aether_android_bridge_render()`。

## 完整性

`SHA256SUMS` 覆盖 `upstream/` 下全部纳入文件；逐条符号映射见 `MAPPING.md`。
