# 00 · 环境基线（本机实测，v2 §2.1）

> 本文件是**实测事实**，不是规格、不是门禁判据。采集入口 `tools/collect_baseline.py`，
> 机器可读原始结果 `evidence/baseline.json`。环境变更后必须重跑并重填本表（v2 §2.1）。
> 本文件不含任何 PASS/FAIL 判定 —— 判据归 §5.6 机检与 §15 验收（遵守 §11.3「不自改门禁」红线）。

## 采集方式

```bash
python3 tools/collect_baseline.py            # 写 evidence/baseline.json + 打印人读表
python3 tools/collect_baseline.py --human-only   # 只打印，不写文件
```

脚本只读 `/proc`、`/sys`、环境变量与仓库内文件，无第三方依赖、无 root 要求、无绝对路径、
不联网。除 `generated_at` 外，同一环境两次采集的 JSON 逐字节一致（§2.5 可复现）。

## 实测表（2026-11-13 采集，已按本机重填）

| 项 | v2 表内原值（参考环境） | **本机实测值** | 出处 / 测量方法 |
| --- | --- | --- | --- |
| 架构 / 内核 | （未列） | `x86_64` / `6.6.117-45.11.6.tl4.x86_64` | `platform.machine()` / `platform.release()` |
| 虚拟化 | 无 `/dev/kvm`，无 vmx/svm | **无 `/dev/kvm`，vmx=0，svm=0** → `tcg-only` | `/proc/cpuinfo` 子串计数 + `os.path.exists` |
| CPU | 2 核 | **32 逻辑核**（AMD EPYC 9K84 96-Core） | `os.cpu_count()` / `/proc/cpuinfo` model name |
| 内存 | 3 GB 可用 | **总 129308476 kB（≈123 GB）／可用 86994928 kB（≈83 GB）** | `/proc/meminfo` 的 `MemTotal` / `MemAvailable` |
| 磁盘 | 9.7 GB 可用 | **总 274877906944 B（256 GB）／可用 165761974272 B（≈154 GB）** | `shutil.disk_usage(仓库挂载点)` |
| 权限 | 非 root，`/usr`、`/run` 不可写 | **uid=0（root）**，可写 `/usr`=1、可写 `/run`=1 | `os.geteuid()` / `os.access(..., W_OK)` |
| 用户态画布 | 720×1280 fbdev | 720×1280（`aether-tokens.h` 单一参数源） | 参数源头文件，非环境基线 |

## 工具链（实测齐备情况）

| 工具 | 实测 | 用途（S1/S2） |
| --- | --- | --- |
| `gcc` / `cc` / `clang` | `/usr/bin/gcc`、`cc`、`clang` | B 轨 x86 原生编译 |
| `aarch64-linux-gnu-gcc` | `/usr/bin/aarch64-linux-gnu-gcc` | A 轨内核/用户态交叉编译 |
| `qemu-system-aarch64` | `/usr/bin/qemu-system-aarch64` | A 轨 TCG 整机 |
| `mkfs.ext4` / `debugfs` / `e2fsck` | `/usr/sbin/` 三者齐备 | 自建根文件系统 |
| `objcopy` | `/usr/bin/objcopy` | 字体 blob 静态链入 |
| `pkg-config`（libdrm） | `/usr/bin/pkg-config`，`libdrm` 存在 | DRM 显示后端 |
| `git` / `python3` | 齐备 | 版本管理 / 工具链 |

## 仓库内资产（实测）

| 资产 | 实测 | 说明 |
| --- | --- | --- |
| 内核镜像 | **无**（`Image` / `bzImage` / `kernel-android.img` 均未命中） | 对应 v2 修正清单 #3：必须从源码构建或提供带校验和的获取脚本 |
| 系统盘 | **无**（`system.qcow2` / `rootfs.ext4` / `system.img` 均未命中） | 同上，本阶段需自建 |
| 矢量字体 | `guest/assets/aether-font.ttf`（存在） | 已自带，符合 §18「自带优先」 |
| 交叉构建脚本 | `guest/build-aarch64.sh`（存在） | 存在硬编码绝对路径问题，见下方差异第 3 条 |

## 与 v2 参考环境的差异及处理方式（如实记录）

1. **无硬件虚拟化这一条在本机同样成立**：vmx/svm 计数为 0、`/dev/kvm` 不存在 →
   `effective_mode = tcg-only`。因此 v2 最重要的结论「A 轨只能用 TCG 纯软件模拟、
   ARM64 整机启动按分钟计」**仍然成立**，双轨拆分（§2.6）必须保留，不得因为本机 CPU 多
   就合并验收。硬件加速仍按 §2.1 列为「若将来环境支持则启用」，不写进验收前提。

2. **资源结论改写**：v2 表基于「2 核 / 3 GB / 9.7 GB」推出「资源不达标 → 默认串行单实例
   （§2.3）」「并跑两台会互相拖死」「渲染帧率被硬限制」。本机 32 核 / 123 GB / 154 GB，
   资源约束不成立。按 §2.3「资源达标时：可并跑多实例」与 §2.1「环境变了要重填」，本阶段
   允许并行 A/B 双轨取证；但**并行结果不得伪造**，是否真正并行由用例快照与日志时间戳证明，
   不靠口头声明。v2 原表中的「影响」列仅适用于参考环境，本表不沿用其结论。

3. **权限结论不放宽**：本机是 root 且可写 `/usr`、`/run`，但 v2 §2.4 明确「不许依赖机器
   绝对路径与系统目录写权限」、§2.1 要求「服务 socket 与素材路径必须可配置」。因此
   **不因为本机可写就硬编码 `/run`、`/usr`**。所有运行产物落到仓库内可配置目录
   （`AETHER_OUT` 环境变量，默认仓库相对路径），换一台非 root 云电脑也能跑。

4. **内核与系统盘必须自建**：实测仓库内无内核镜像、无系统盘，且历史构建脚本
   （`guest/build-host.sh`、`build-aarch64.sh`）把输出硬编码到 `/root/aether-archive/...`。
   按 v2 修正清单 #3 与 §10.3，本阶段的一键构建必须**从源码构建内核 + 自建根文件**，
   或提供带校验和的获取脚本；禁止机器绝对路径。此项列为 S1 待办，不虚报为已完成。

## 变更记录（§10.2 每个数值有出处、有变更记录）

| 日期 | 变更 | 前后对比 / 证据 |
| --- | --- | --- |
| 2026-11-13 | 首次采集并按本机重填 §2.1 表 | v2 参考值 2 核/3GB/9.7GB/非 root → 本表 32 核/123GB/154GB/root；原始输出见 `evidence/baseline.json`，采集脚本输出见 `evidence/round-001/baseline.stdout.txt` |
