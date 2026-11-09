# AGENTS.md — AetherOS Phone 客机工程指引

面向接手本工程的 AI Agent。本目录是 **AetherOS 手机版的客机用户态源码**，不是 PC
发行版：项目只存在手机形态，PC 桌面方案已彻底废弃，界面内不绘制任何鼠标指针。

## 一句话定位

`AetherOS Phone` = aarch64 Linux 6.6.70 内核（由 `aether-vm` 工程提供）+ 本目录纯 C
静态链接用户态（init / 守护 / 合成器 / 看门狗 / 手机外壳），由 Android APK 内的 QEMU
引擎加载 `system.qcow2` 开机，合成器直接写 `/dev/fb0` 出桌面。

## 构建链路（顺序不可乱）

1. `build-aarch64.sh` — aarch64 静态交叉编译全部用户态，产物默认落在
   `/root/aether-archive/aether-aarch64-build/root/`。
   系统盘注入源固定为同级的 `root-full/sbin/aether-compositor`；改完源码后必须用
   `AETHER_BUILD=/root/aether-archive/aether-aarch64-build/root-full bash build-aarch64.sh`
   重编，否则 `make_system_v6.sh` 仍注入旧二进制。
2. `/workspace/aether-vm/make_system_v6.sh` — 从干净的 v5 基线盘重打 v6：注入前先
   `e2fsck -fy` 清空 ext4 journal，注入后复修复检，并对 7 个文件做 SHA-256 + 权限
   硬校验。**直接对带 `needs_recovery` 的盘做 `debugfs -w` 会导致客机 exec 崩溃
   （rc=139）**，这是历史根因，不要绕过前置 e2fsck。
3. `/workspace/aether-vm/host/run_v6.sh` — 编译自研 `aether-vm`，用 QEMU TCG 真实
   开机，串口日志与 9p 导出的逐帧 PPM 落在 `out/run-v6/`，脚本自动转 PNG。

`build-host.sh` 只做 host x86_64 原生静态编译验证（需 `libdrm-dev`），不产出可启动镜像。

## 服务模型（aether-init）

`aether-init`（PID 1）解析 `/etc/aether/init.d/<svc>`，格式为极简 `key=value`：
`name= / exec= / depends= / restart=(never|on-failure|always)`。
按依赖拓扑启动服务、阻塞回收子进程、按策略重启。
服务状态写入 `/run/aether/services.tsv`（**必须在 tmpfs /run 挂载之后创建
`/run/aether`**，否则会被新 tmpfs 覆盖）。

## 关键源文件

| 文件 | 职责 |
|------|------|
| `src/aether-init.c` | PID 1 服务管理器 |
| `src/aetherosd.c` | 系统守护 / AetherBus（`/run/aetheros.sock`） |
| `src/aether-compositor.c` | 合成器主循环：触摸输入 + IPC + 渲染调度 |
| `src/aether-display.c` / `aether-drm.c` | fbdev / KMS 双后端 |
| `src/aether-render.c/h` | 软渲染原语：填充 / 文字 / 图标 / 圆角 / 模糊 |
| `src/aether-vfont.c/h` + `stb_truetype.h` | 内嵌矢量 TrueType 栅格化 |
| `src/aether-glass.c/h` | Liquid Glass 光学与合成 |
| `src/aether-glass-spec.h` | 上游组件规范固化的 C 常量与层级规则 |
| `src/aether-phone.c` | 唯一外壳实现（锁屏 / 主屏 / 导航 / 通知 / 多任务） |
| `src/aether-desktop.h` | 桌面服务内核共享 API（内部命名，非 PC 产品形态） |

## 常见坑（已踩过，勿重蹈）

- **平面类型属性名是小写 `type`**（内核属性），不是 `TYPE`；否则 `get_prop_id` 查不到、
  `DRM_PLANE_TYPE_PRIMARY` 永远不匹配 → “未找到主平面”。代码已加兜底（取覆盖本 CRTC 的首平面）。
- **`/run/aether` 必须在 `mount tmpfs /run` 之后创建**，否则目录被新 tmpfs 覆盖，状态文件写不进去。
- 合成器 `poll` 数组要按实际 fd 数动态分配，**不可写死 4**（历史上第 5 个输入 fd 越界，
  触发 `__poll_chk` 栈溢出 SIGABRT）。
- 渲染后**必须回到 `poll` 服务 IPC**，不可 `continue` 跳过，否则看门狗心跳连不上会被误杀。
- 截图把整缓冲一次性 `fwrite`，**不可逐像素写**（9p + TCG 下极慢/卡死）。
- 矢量字体 blob 由 `objcopy` 生成，链接 compositor 必须带 `-Wl,-z,noexecstack`，
  否则报 `missing .note.GNU-stack section implies executable stack`。
- `glass_apply_glass_color()` 的彩度判定必须基于上游原始 `glassColor` 的 `tr/tg/tb`，
  不能用已着色的中间值，否则无色玻璃会被误判为彩色。

## 输入约定

只识别 `EV_ABS`（`ABS_X` / `ABS_Y`）+ `BTN_TOUCH` 与 `EV_KEY`；`EV_REL` / `REL_X` /
`REL_Y` / `BTN_LEFT` 路径已整体移除。`status` IPC 返回 `touch:[x,y]`，不再是 `cursor`。
渲染层只记录 `touch_x/touch_y` 供分轴累积与命中测试，不绘制任何指针。

## 渲染验证

`run_v6.sh` 结束后查看 `out/run-v6/frames/*.png` 与 `out/run-v6/serial.log`；
断言串口出现 `SELFTEST_DONE=1`、compositor 显示 `backend=1`（fbdev），且无
`rc=139` / `segfault` / `EXT4-fs error` / `Structure needs cleaning` 命中。
