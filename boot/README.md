# 整机引导链路（A 轨）

本文件定义 AetherOS 的整机引导链路：在云计算机上以 QEMU ARM64 TCG 软件模拟，从 Linux
内核引导到自研用户态服务编排、virtio-gpu 图形上屏、安全引擎开机自检与开机诊断，逐环留存
串口证据，并由 QEMU QMP 抓取真实锁屏首帧作像素级断言。整机链路用于验证
「引导 → 内核 → 挂载根文件系统 → PID 1 → 服务编排 → 锁屏上屏 → 安全自检 → 开机诊断」是否连通。

> 范围（如实标注）：当前 A 轨为**最小可启动整机**——init 编排拉起自研合成器（上屏真实锁屏界面）、
> 安全引擎开机自检、开机诊断；并非带完整桌面的整机。早期自研内核已移出仓库（**非逐项修复**，
> V1–V17 清单随代码归档），产品内核为 Linux。完整桌面、软键盘与输入生效待后续阶段。

## 链路组成

| 环节 | 内容 | 载体 |
| --- | --- | --- |
| 内核 | Linux 6.6.70（ARM64），从源码交叉编译 | `kernel-6.6.70.config` + `build.sh` |
| 根文件系统 | 自建 ext4 根盘：aarch64 BusyBox 基座 + `guest/src` 交叉编译产物 + `secure/` 安全引擎 + init 服务清单 + 开机诊断 | `rootfs/` + `assets/busybox-aarch64` |
| PID 1 | 自研 `aether-init`，负责挂载与服务编排 | `build/aarch64/root/sbin/aether-init` |
| 服务 | aetherosd、aether-compositor、aether-security、bootdiag（声明式服务清单，按依赖拓扑启动） | `rootfs/init.d/` |
| 显示 | virtio-gpu DRM/fb 设备，自研合成器竖屏画布上屏 | `guest/src/aether-compositor.c` + `run-qemu.sh` |
| 安全 | aarch64 静态安全引擎随开机完成设备自证自检 | `secure/build-aarch64.sh` + `rootfs/etc/secure-selftest.sh` |
| 验证 | 串口日志 + QMP 真实首帧像素统计 + 开机门禁机检 | `run-qemu.sh` + `tools/capture_frame.py` + `tools/gate_boot.py` |

外部资产（Linux 内核源码 tarball、aarch64 BusyBox 静态基座）以 `ASSETS.sha256` 校验清单
管理；获取脚本先校验本地缓存，缺失时按镜像优先级下载并校验。所有路径由脚本自身位置推导或
经环境变量注入，构建产物落在 `build/aarch64/`，由 `.gitignore` 排除。

## 已取证的冷启动链路

```
QEMU aarch64 (-M virt, TCG, virtio-gpu-pci)
   → Linux 6.6.70 Image 引导（从源码交叉编译）
   → 挂载自建 ext4 根文件系统（virtio-blk /dev/vda）
   → virtio-gpu DRM probe → /dev/dri/card0 + fbdev emulation /dev/fb0
   → 自研 PID 1 aether-init
   → 服务编排（aetherosd / aether-compositor / aether-security / bootdiag）
   → 合成器打开显示，竖屏画布上屏（720x1280 → 1280x800 letterbox）
   → 安全引擎设备自证自检（attest 签名 + 系统摘要锚）
   → 开机诊断输出真实内核数据源（/proc/version、/proc/meminfo、/proc/uptime）
   → QEMU QMP screendump 抓取真实图形首帧（像素级判定）
   → 开机门禁逐环节断言串口证据与首帧统计
```

串口证据中可核实的标记：`Linux version 6.6.70`、`EXT4-fs (vda): mounted filesystem`、
`VFS: Mounted root`、`Run /init as init process`、`AetherOS 启动 (PID 1)`、
`加载了 4 个服务`、`所有可启动服务已拉起，系统就绪`、`显示打开 1280x800 backend=1`、
`AETHEROS_SECURE_SELFTEST: arch=aarch64 kernel=6.6.70 attest=OK`、
`AETHEROS_SECURE_READY=1`、`fb0=yes dri=card0`、`AETHEROS_BOOT_DIAG_DONE=1`。

## 构建与运行

```bash
bash boot/fetch-assets.sh     # 下载并校验外部资产（Linux 内核源码 tarball 等）
bash boot/build.sh            # 5 步：内核 + aarch64 用户态 + 安全引擎 + ext4 根盘，产物落 build/aarch64/
bash boot/run-qemu.sh         # QEMU TCG 冷启动，串口日志与真实首帧写到 evidence/（仓库内）
python3 tools/gate_boot.py    # 开机门禁：逐环节断言串口证据 + 图形帧像素统计（10/10）

# 六种一键操作中的 A 轨入口
bash scripts/aether.sh start --track A
```

> 环境依赖（复现前提）：`aarch64-linux-gnu-gcc`、`qemu-system-aarch64`、`make` 与交叉 libc
> 基础库。无 root / apt 不可用的环境需先具备交叉工具链与 QEMU（约 30–60 分钟构建内核）；本仓库
> 提供脚本但不打包 41 MB 内核镜像与 256 MB 根盘（依据约定不入库二进制构建产物）。`evidence/` 目录
> 随仓库分发（仅说明与可复现路径），实际取证产物由上述脚本现场生成，不入库。

`boot/build.sh` 分五步：外部资产获取与校验、Linux 内核从源码交叉编译、`guest/src` 同一份
源码交叉编译、`secure/` 同一份源码交叉编译、ext4 根盘组装。任一环节失败即中止，不产出半成品
镜像。`boot/run-qemu.sh` 在开机诊断完成后等待合成器上屏，通过 QEMU QMP `screendump` 抓取
virtio-gpu 真实 scanout，连续采样两帧并做颜色/非背景像素判定，全部达标才结束本次取证。

可用环境变量：`AETHER_BUILD`（构建产物目录）、`JOBS`（内核并行编译数）、
`ROOTFS_SIZE`（根盘大小 MB）、`AETHER_EVIDENCE`（证据目录）、`BOOT_TIMEOUT`（启动超时秒）。

## 与上层代码的关系

用户态源码统一取自 `guest/src`，安全引擎源码统一取自 `secure/`：调试环境的原生构建、
整机链路的 aarch64 交叉编译、真机移植使用同一份源码，仅构建目标与硬件接口层实现不同。
接口层定义见 `l2/include/`。

## 当前状态

| 组件 | 状态 |
| --- | --- |
| Linux 6.6.70 内核配置与源码构建、aarch64 用户态与安全引擎交叉编译 | 已入仓 |
| 自建 ext4 根文件系统、init 服务清单、开机诊断与安全自检脚本 | 已入仓 |
| QEMU virtio-gpu 启动脚本、QMP 抓帧、开机门禁机检、外部资产 SHA-256 校验清单 | 已入仓 |
| 冷启动到锁屏上屏、安全自检、开机诊断的取证 | 已取证，门禁 10/10 通过 |
| 真实锁屏首帧（QEMU QMP 抓取 + 像素级判定） | 已取证，连续两帧稳定 |
| 注入输入改变画面、逐帧取证 | 后续阶段，尚未取证 |
| `scripts/aether.sh` 六种一键与 A 轨前置检查 | 已入仓 |

## 工程边界

- 整机链路证明从内核引导到图形首帧的连通性，不用于帧率、动效手感与长稳验收；逐帧交互验证属后续阶段。
- 图形首帧来自 QEMU QMP 对 virtio-gpu scanout 的真实抓取，非合成器自检打印；合成器以 fbdev
  后端写入 DRM fbdev emulation 提供的 `/dev/fb0`（`virtio_gpudrmfb`）。
- 安全引擎随开机完成一次设备自证自检（运行根置于 tmpfs，不留持久数据），尚未接入开机硬门与
  常驻服务；完整服务化集成属后续阶段。
- 触摸、音频、相机、基带、TEE/eFuse 与刷真机属于真机阶段，调试环境使用 QEMU virtio 仿真设备。
- 软键盘（含中文输入）是整机闭环的一票否决验收项，属后续阶段，当前未计入已通过。
- 内核源码 tarball 不随仓库分发，由 `fetch-assets.sh` 按 `ASSETS.sha256` 校验后下载；
  aarch64 BusyBox 静态基座随仓库分发，采用 BusyBox GPL-2.0 许可，来源与归属见
  [`../guest/OPENSOURCE-ATTRIBUTION.md`](../guest/OPENSOURCE-ATTRIBUTION.md)。
