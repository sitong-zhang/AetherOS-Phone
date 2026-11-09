# 安卓兼容层逐条映射（MAPPING.md）

上游文件 : 符号 / 规则 → AetherOS 实现符号。与液态玻璃 MAPPING.md 同规格。

## A 节 — Waydroid 容器启动（GPL-3.0）

| 上游 | 符号 / 规则 | AetherOS |
|---|---|---|
| `waydroid/lxc.py:85` | `lxc.mount.entry = none dev/pts devpts ...` | `mount_in_rootfs("dev/pts","devpts",...)` |
| `waydroid/lxc.py:75-77` | `/dev/binder`→`dev/binder`、vnd/hwbinder | `probe_caps().binder` 检测 + `mount_in_rootfs("dev/binderfs","binder",...)` |
| `waydroid/lxc.py:422` | `lxc-start ... -n waydroid -- /init` | `container_child()` 末尾 `execve("/init", ...)` |
| `waydroid/lxc.py:444-452` | `ANDROID_ROOT=/system` / `ANDROID_DATA=/data` | `container_child()` 的 `envp[]` |
| `waydroid/lxc.py:422` | 命名空间隔离（LXC user/pid/uts/net/mount/ipc） | `unshare(CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC|CLONE_NEWPID)` |
| `waydroid/lxc.py:190` | 挂载路径合法性/属主检查 | `bind_dev()` 只接受固定 `/dev/` 源路径，不接收外部任意路径 |
| `waydroid/lxc.py` | `lxc.mount.entry ... bind,create=file,optional` | `bind_dev()` 先建空文件再 `MS_BIND`；`mount_in_rootfs()` 失败按 optional 跳过 |
| `waydroid/drivers.py:38-85` | binder 节点存在性探测 + binderfs 分配 | `probe_caps()` 检查 `/dev/binderfs`、`/dev/binder`、`/dev/binderfs/binder` |
| 用户路线第 2 点 | 绑定 `/dev/fb0` 与 uinput | `bind_dev("/dev/fb0","dev/fb0")` + `bind_dev("/dev/uinput","dev/uinput")` |

## B 节 — AOSP AVF / pKVM 检测（Apache-2.0）

| 上游 | 符号 / 规则 | AetherOS |
|---|---|---|
| `aosp-avf/README` | `getSystemService(VirtualMachineManager.class)` 非空即支持 | `AvfCapability.probe()` 反射 `Class.forName(VMM_CLASS)` + `getSystemService` |
| `aosp-avf/README` | `getCapabilities()` / `CAPABILITY_PROTECTED_VM` / `CAPABILITY_NON_PROTECTED_VM` | 分级 `NONE..APP_USABLE`（0..4），`PROTECTED` 对应受保护 VM |
| `aosp-avf/README` | `hasSystemFeature(FEATURE_VIRTUALIZATION_FRAMEWORK)` 任意 API 可用 | `pm.hasSystemFeature("android.software.virtualization_framework")`（字面量兼容 minSdk 21） |
| `aosp-avf/README` | `VirtualMachineConfig.Builder.setProtectedVm / setPayloadBinaryName / build` | `AvfLauncher.tryStartAvf()` 反射同序调用 |
| `aosp-avf/README` | `vmm.getOrCreate(name, config)` / `vm.run()` | `AvfLauncher.tryStartAvf()` 反射 getOrCreate + run |
| `aosp-avf/README` | `@SystemApi` + `MANAGE_VIRTUAL_MACHINE`（Android14 特权/15 预装） | `AvfCapability.probe()` 用 `checkCallingOrSelfPermission` 判定，缺权限不升 `APP_USABLE` |
| `pkvm-detection` | `ro.boot.hypervisor.protected_vm.supported` / `.vm.supported` | `getProperty()` 反射 SystemProperties 或 getprop |
| `pkvm-detection` | `/dev/kvm`、`/apex/com.android.virt/bin/vm` | `exists()` 探测 |
| `pkvm-detection` | 受保护 VM 需 arm64 / API 34+ | `Build.VERSION.SDK_INT >= 34` 门控 |
| `AvfCapability.kernelArgs` | 探测结果透传客机 | `aether.avf.level/name/pkvm/appapi` 写入 vm.conf.final 的 append → 客机读 `/proc/cmdline` |

### 三条硬约束（决定启动路径，不得曲解）

1. 普通第三方 APK 无 `MANAGE_VIRTUAL_MACHINE` → `probe < APP_USABLE`；
2. AVF 面向 headless / Microdroid，**不提供图形 HAL**，无法承载带桌面的完整客机；
3. 本工程载荷是 `kernel.img + system.qcow2` 完整 Linux 盘，**非 Microdroid payload**。

因此 `AvfLauncher.decide()` 默认且实际生效路径恒为 `MODE_QEMU_TCG`（已验证的
SDL2 + QEMU aarch64 TCG），AVF 分支为 capability-gated 反射入口，缺 payload
（`AvfPayload.hasPayload`）即降级。这是**如实实现**，不是假称 AVF 已跑通。

## C 节 — Linux UAPI 输入桥（GPL-2.0 WITH Linux-syscall-note）

| 上游 | 符号 / 规则 | AetherOS |
|---|---|---|
| `uinput.h` | `UI_SET_EVBIT` / `UI_SET_ABSBIT` / `UI_SET_KEYBIT` | `ensure_uinput()` ioctl |
| `uinput.h` | `struct uinput_abs_setup` + `UI_ABS_SETUP` | ABS_X/ABS_Y 范围 0..W-1 / 0..H-1 |
| `uinput.h` | `struct uinput_setup` + `UI_DEV_SETUP` + `UI_DEV_CREATE` | 设备名 `aether-android-touch`，BUS_VIRTUAL |
| `uinput.h` | `UI_DEV_DESTROY` | `aether_android_bridge_shutdown()` |
| `input-event-codes.h` | `EV_ABS=0x03` / `EV_KEY=0x01` / `EV_SYN=0x00` | 事件注入序列 |
| `input-event-codes.h` | `ABS_X=0` / `ABS_Y=1` / `BTN_TOUCH=0x14a` / `SYN_REPORT=0` | `aether_android_bridge_input()` |
| 单元测试 | — | `tools/test_android_compat.c` 校验这些 UAPI 常量 |

## D 节 — 图形桥协议（共享内存）

| 概念 | Waydroid | AetherOS |
|---|---|---|
| 生产者 | SurfaceFlinger / hwcomposer 写 Wayland buffer | 容器内图形 HAL 写 `/run/aether/android.fb` |
| 共享机制 | memfd_create + wl_shm / dmabuf | `open+ftruncate+mmap(MAP_SHARED)` |
| 帧元数据 | wl_buffer / IWaydroidDisplay setLayerSize | 64B 头：magic/seq/w/h/stride/format/flags/win_* |
| 消费者 | waydroid-weston 合成到窗口 | `aether_android_bridge_render()` 缩放贴进桌面浮窗 |
| 不抢全屏 | 多窗口 freeform | 浮窗几何 `win_x/y/w/h`，只覆盖桌面局部 |
| 可见性 | surface attach | `flags & AANDROID_FB_FLAG_VISIBLE`；容器未运行恒 0 |

## E 节 — 商店分类与降级提示

| 用户路线 | AetherOS |
|---|---|
| 第 5 点：商店「安卓应用」分类 | `aether-store.c::store_android()`（view=3）+ 首页分类入口 |
| 第 5 点：`pm install` / `am start` | `aether-android.c::container_exec()` 转发 `/system/bin/pm`、`/system/bin/monkey`（拉起主 Activity） |
| 第 6 点：pKVM 不可用提示「需要更高版本系统」 | `AANDROID_HINT_PKVM` 常量 + 商店分类页显示 + 服务日志 |
| 第 6 点：AetherOS 本体照常用 | 桥在无容器时零副作用（不绘制/不拦截输入），39 帧自检不受影响 |

## F 节 — 诚实边界（不实称已完成）

- LineageOS arm64 rootfs 是 GB 级外部镜像，**不在 APK / 18MB 系统盘内**；
- 客机内核未必编入 `CONFIG_ANDROID_BINDERFS` / `CONFIG_INPUT_UINPUT`；
- 容器内 SurfaceFlinger 的 hwcomposer HAL 需 AOSP 侧配套，当前未接通；
- 因此真机（QEMU TCG）闭环验证的是：**能力探测 → 依赖缺失 → unavailable 状态 →
  「需要更高版本系统」提示 → AetherOS 本体 39 帧自检照常通过**这一完整降级链路；
  不声称「百万安卓 APK 已在桌面打开」。
