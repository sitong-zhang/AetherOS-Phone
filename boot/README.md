# 整机引导链路（A 轨）

本文件定义 AetherOS 的整机引导链路：在云计算机上以 QEMU ARM64 TCG 软件模拟，从 Linux
内核引导到自研用户态服务编排与开机诊断，并逐环留存串口证据。整机链路用于验证
「引导 → 内核 → 挂载根文件系统 → PID 1 → 服务编排 → 开机诊断」是否连通。

## 链路组成

| 环节 | 内容 | 载体 |
| --- | --- | --- |
| 内核 | Linux 6.6.70（ARM64），从源码交叉编译 | `kernel-6.6.70.config` + `build.sh` |
| 根文件系统 | 自建 ext4 根盘：aarch64 BusyBox 基座 + `guest/src` 交叉编译产物 + init 服务清单 + 开机诊断 | `rootfs/` + `assets/busybox-aarch64` |
| PID 1 | 自研 `aether-init`，负责挂载与服务编排 | `build/aarch64/root/sbin/aether-init` |
| 服务 | aetherosd、bootdiag（声明式服务清单，按依赖拓扑启动） | `rootfs/init.d/` |
| 显示 | virtio-gpu 首帧 | 后续阶段，尚未取证 |
| 验证 | 串口日志 + 开机门禁机检 | `run-qemu.sh` + `tools/gate_boot.py` |

外部资产（Linux 内核源码 tarball、aarch64 BusyBox 静态基座）以 `ASSETS.sha256` 校验清单
管理；获取脚本先校验本地缓存，缺失时按镜像优先级下载并校验。所有路径由脚本自身位置推导或
经环境变量注入，构建产物落在 `build/aarch64/`，由 `.gitignore` 排除。

## 已取证的冷启动链路

```
QEMU aarch64 (-M virt, TCG)
   → Linux 6.6.70 Image 引导（从源码交叉编译）
   → 挂载自建 ext4 根文件系统（virtio-blk /dev/vda）
   → 自研 PID 1 aether-init
   → 服务编排（aetherosd / bootdiag）
   → 开机诊断输出真实内核数据源（/proc/version、/proc/meminfo、/proc/uptime）
   → 开机门禁逐环节断言串口证据
```

串口证据中可核实的标记：`Linux version 6.6.70`、`EXT4-fs (vda): mounted filesystem`、
`VFS: Mounted root`、`Run /init as init process`、`AetherOS 启动 (PID 1)`、
`加载了 2 个服务`、`所有可启动服务已拉起，系统就绪`、`AETHEROS_BOOT_DIAG_DONE=1`。

## 构建与运行

```bash
bash boot/build.sh            # 内核 + aarch64 用户态 + ext4 根盘，产物落 build/aarch64/
bash boot/run-qemu.sh         # QEMU TCG 冷启动，串口日志写本地归档
python3 tools/gate_boot.py    # 开机门禁：逐环节断言串口证据

# 六种一键操作中的 A 轨入口
bash scripts/aether.sh start --track A
```

`boot/build.sh` 分四步：外部资产获取与校验、Linux 内核从源码交叉编译、`guest/src` 同一份
源码交叉编译、ext4 根盘组装。任一环节失败即中止，不产出半成品镜像。`boot/run-qemu.sh`
检测到诊断完成标记后结束本次取证，不模拟启动成功。

可用环境变量：`AETHER_BUILD`（构建产物目录）、`JOBS`（内核并行编译数）、
`ROOTFS_SIZE`（根盘大小 MB）、`AETHER_EVIDENCE`（证据目录）、`BOOT_TIMEOUT`（启动超时秒）。

## 与上层代码的关系

用户态源码统一取自 `guest/src`：调试环境的原生构建、整机链路的 aarch64 交叉编译、真机移植
使用同一份源码，仅构建目标与硬件接口层实现不同。接口层定义见 `l2/include/`。

## 当前状态

| 组件 | 状态 |
| --- | --- |
| Linux 6.6.70 内核配置与源码构建、aarch64 用户态交叉编译 | 已入仓 |
| 自建 ext4 根文件系统、init 服务清单、开机诊断脚本 | 已入仓 |
| QEMU 启动脚本、开机门禁机检、外部资产 SHA-256 校验清单 | 已入仓 |
| 冷启动到开机诊断的串口取证 | 已取证，门禁 6/6 通过 |
| virtio-gpu 图形首帧、注入输入改变画面、逐帧取证 | 后续阶段，尚未取证 |
| `scripts/aether.sh` 六种一键与 A 轨前置检查 | 已入仓 |

## 工程边界

- 整机链路只证明链路连通，不用于帧率、动效手感与长稳验收；交互与像素验证由原生上层轨道承担。
- 用户态合成器当前以 fbdev 路径构建，QEMU virt 未提供 `/dev/fb0`（诊断输出 `fb0=no`），
  因此 A 轨不虚报图形首帧。
- 触摸、音频、相机、基带、TEE/eFuse 与刷真机属于真机阶段，调试环境使用 QEMU virtio 仿真设备。
- 软键盘（含中文输入）是整机闭环的一票否决验收项，属后续阶段，当前未计入已通过。
- 内核源码 tarball 不随仓库分发，由 `fetch-assets.sh` 按 `ASSETS.sha256` 校验后下载；
  aarch64 BusyBox 静态基座随仓库分发，采用 BusyBox GPL-2.0 许可，来源与归属见
  [`../guest/OPENSOURCE-ATTRIBUTION.md`](../guest/OPENSOURCE-ATTRIBUTION.md)。
