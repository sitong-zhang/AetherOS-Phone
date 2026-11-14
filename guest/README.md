# AetherOS Phone — 客机用户态工程（历史留痕 · 旧 APK 口径）

> ⚠️ **口径更正（2026-11-14）**：本文件开头及正文描述的是**已被推翻的旧交付形态**
> ——「内置于 Android APK 的手机系统」。当前产品是**真 Linux 手机 OS、刷开发板/谷歌真机、
> 不做 APK**（见根目录 `AGENTS.md` 与 `docs/SYSTEM-BLUEPRINT.md`）。
> 本文件仅因记录「可复用的自研用户态上层」而有价值，**交付形态描述不作当前依据**。
> 此处的 `guest/src` 源码本身仍是可复用资产（同一份上层源码原则）。

AetherOS Phone 是内置于 Android APK（包名 `dev.aether.vm`）中的手机操作系统。宿主 APK
通过 QEMU 引擎加载 aarch64 客机 Linux 内核与 `system.qcow2` 系统盘；开机后由本工程
提供的纯 C 用户态直接绘制桌面，无需 Android HAL。

本项目**只存在手机形态**：PC 桌面方案已彻底废弃，界面内不绘制任何鼠标指针，输入仅
识别触摸屏（`EV_ABS` + `BTN_TOUCH`）与键盘（`EV_KEY`）。

## 组成

| 模块 | 职责 |
|------|------|
| `src/aether-init.c` | PID 1：挂载基础文件系统、按依赖启动并守护服务 |
| `src/aetherosd.c` | 系统守护，经 `/run/aetheros.sock` 暴露 AetherBus |
| `src/aether-compositor.c` | 显示服务端：触摸输入扫描 + IPC + 渲染调度 |
| `src/aether-display.c` / `aether-drm.c` | fbdev / KMS 双显示后端 |
| `src/aether-render.c` / `aether-vfont.c` | XRGB8888 逐像素软渲染 + 内嵌矢量字体 |
| `src/aether-glass.c` | Liquid Glass 材质（上游开源公式的 C 重实现） |
| `src/aether-phone.c` | 手机外壳：锁屏 / 状态栏 / 主屏 / 导航 / 通知 / 控制中心 / 多任务 / 应用商店 |
| `src/aether-apps*.c` 等 | 内置应用 |
| `tools/`、`assets/` | 字体生成、数学测试、壁纸与字体资产 |

矢量字体为 Noto Sans CJK SC 的 TrueType 子集，由 `tools/gen_vector_font.py` 离线生成，
用 `objcopy` 静态链入合成器，运行时零外部文件依赖。

## Liquid Glass 材质来源

光学公式与组件规范取自两个开源仓库，源码副本、许可证与逐条对照表均随工程保留：

- `Kyant0/AndroidLiquidGlass`（Apache-2.0）：SDF / 折射 / 色散 / 高光，光学基准。
- `sdegenaar/liquid_glass_widgets`（MIT）：全套控件组件规范、参数默认值、质量档。

见 `third_party/liquid-glass/SOURCE.md`、`MAPPING.md`、`SHA256SUMS`，以及固化后的
C 常量 `src/aether-glass-spec.h`。上游为 Kotlin / AGSL / Dart / GLSL 并依赖 GPU 运行时，
本工程客机为纯 CPU 逐像素渲染，因此采用「上游源码原样副本 + 映射表 + C 忠实重实现」
的形态，而非整段编译。

## 构建

```bash
# host 原生静态编译（x86_64），用于快速编译验证；需 libdrm 开发头文件
bash build-host.sh

# 真机构建：aarch64 静态交叉编译，产物供 ext4 系统盘注入
bash build-aarch64.sh

# 玻璃数学单元测试
gcc -O2 -I src -o /tmp/test_glass_math tools/test_glass_math.c src/aether-glass.c -lm && /tmp/test_glass_math
```

系统盘生成与真机闭环由 `aether-vm` 工程提供：`make_system_v6.sh` 把 aarch64 合成器
注入 ext4 系统盘并做 SHA-256 / 权限 / `e2fsck` 校验，`host/run_v6.sh` 用 QEMU TCG
真实开机、逐帧导出取证。

## 输入

合成器只扫描具备 `EV_ABS` 的触摸设备与 `EV_KEY` 键盘设备；`EV_REL` 相对位移设备
（鼠标）不纳入。IPC 的 `move` / `click` / `down` / `up` 仅用于自动化取证时注入触点
坐标，不产生任何指针绘制。

## 已知约束

- 沙箱中以 QEMU TCG 软件模拟运行，无 `/dev/kvm`；启动与渲染正常，速度偏慢。
- 客机显示走 `/dev/fb0` fbdev，默认 720×1280，支持 `AETHER_RES=WxH` 覆盖。
- 界面整改按移动端视觉规范持续推进，当前进度以工程内源码与取证帧为准。
