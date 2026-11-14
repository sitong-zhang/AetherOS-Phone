# AetherOS 用户态上层工程

`guest/` 是 AetherOS 自研用户态上层源码：init、系统服务、显示服务端、渲染、字体、图标、
桌面与系统应用。该层以纯 C 实现，在 Linux 内核之上直接绘制桌面，不依赖 Android HAL。
同一份源码用于调试环境的原生构建与真机构建（代码同源）。

系统只有手机形态：界面不绘制鼠标指针，输入仅识别触摸屏（`EV_ABS` + `BTN_TOUCH`）与
键盘（`EV_KEY`）。

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

aarch64 交叉编译产物供整机引导链路注入 ext4 根文件系统，构建与启动入口见
`../boot/README.md` 与根目录 `scripts/aether.sh`。

## 输入

合成器只扫描具备 `EV_ABS` 的触摸设备与 `EV_KEY` 键盘设备；`EV_REL` 相对位移设备
（鼠标）不纳入。IPC 的 `move` / `click` / `down` / `up` 仅用于自动化取证时注入触点
坐标，不产生任何指针绘制。

## 已知约束

- 沙箱中以 QEMU TCG 软件模拟运行，无 `/dev/kvm`；启动与渲染正常，速度偏慢。
- 客机显示走 `/dev/fb0` fbdev，默认 720×1280，支持 `AETHER_RES=WxH` 覆盖。
- 界面整改按移动端视觉规范持续推进，当前进度以工程内源码与取证帧为准。
