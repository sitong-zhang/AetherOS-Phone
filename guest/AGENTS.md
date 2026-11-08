# AGENTS.md — AetherOS（电脑版）工程指引

面向接手本工程的 AI Agent。本工程是一个**从零构建的 x86_64 电脑操作系统**，
与手机版（另一独立仓库）**代码互不复用**，仅共享名字 AetherOS 与图标。

## 一句话定位

`AetherOS PC` = Linux 6.6.70 (x86_64) 内核 + 自有用户态（init / 守护 / DRM 合成器 / 看门狗），
可在 QEMU 中以可启动 ext4 镜像运行，桌面渲染出品牌 logo。

## 构建链路（顺序不可乱）

1. `build-pc.sh` — 静态编译 `src/*.c` 到 `build-pc/root/{sbin,bin}`。
   需 `libdrm` 开发头文件（`apt-get install libdrm-dev`）与 `gcc`。
   合成器链接 `-ldrm`（旧版 libdrm 用 `drmModeCreateDumbBuffer` 而非 `drmModeCreateDumb`，代码已适配）。
2. `build-busybox-pc.sh` — 构建静态 busybox 到 `build-pc/busybox/_install`。
   若已有种子（提交/缓存）则跳过下载。
3. `make_rootfs_pc.sh` — 把 busybox + 自有二进制 + 服务配置 + 脚本组装进 staging 目录，
   再用 `mke2fs -d <dir>` 生成 ext4 镜像（**无需 loop 挂载**）。
   已禁用 `metadata_csum` 以便必要时用工具检查。镜像默认 320MB，落到 `/tmp/aether-pc-build/`。
4. `boot-pc.sh` — 启动 QEMU（`-kernel` + `-drive` rootfs + `virtio-gpu` + 键鼠/平板 + 9p 共享）。
   默认无头（`-nographic -serial stdio`），`--gui` 开 gtk 窗口。

## 服务模型（aether-init）

`aether-init`（PID 1）解析 `/etc/aether/init.d/<svc>`，格式为极简 `key=value`：
`name= / exec= / depends= / restart=(never|on-failure|always)`。
它按依赖拓扑启动服务、阻塞回收子进程、按策略重启。
已定义服务：`aetherosd` → `aether-compositor`（依赖前者）→ `aether-watchdog`（依赖前者）、
`bootdiag`（一次性诊断）、`pcshot`（经 9p 导出首屏截图）。
服务状态写入 `/run/aether/services.tsv`（**必须在 tmpfs /run 挂载之后创建 /run/aether**）。

## 关键源文件

| 文件 | 职责 |
|------|------|
| `src/aether-init.c` | PID 1 服务管理器 |
| `src/aetherosd.c` | 系统守护 / AetherBus（`/run/aetheros.sock`） |
| `src/aether-compositor.c` | 合成器主循环：输入 + IPC + 渲染调度 |
| `src/aether-drm.c/h` | KMS/DRM 原子提交、双缓冲（virtio-gpu） |
| `src/aether-render.c/h` | 桌面合成：壁纸/顶栏/logo/文字/光标 |
| `src/aether-watchdog.c` | 心跳探活，连续 3 次超时才杀进程 |
| `src/aether-font.h` | 内嵌 8x16 位图字体（数据资产） |

## 常见坑（已踩过，勿重蹈）

- **平面类型属性名是小写 `type`**（内核属性），不是 `TYPE`；否则 `get_prop_id` 查不到、
  `DRM_PLANE_TYPE_PRIMARY` 永远不匹配 → “未找到主平面”。代码已加兜底（取覆盖本 CRTC 的首平面）。
- **`/run/aether` 必须在 `mount tmpfs /run` 之后创建**，否则目录被新 tmpfs 覆盖，状态文件写不进去。
- 合成器 `poll` 数组要按实际 fd 数动态分配，**不可写死 4**（当年加鼠标 EV_REL 后第 5 个 fd 越界，
  触发 `__poll_chk` 栈溢出 SIGABRT）。
- 渲染后**必须回到 `poll` 服务 IPC**，不可 `continue` 跳过，否则看门狗心跳连不上会被误杀。
- 截图把整缓冲一次性 `fwrite`，**不可逐像素写**（9p + TCG 下极慢/卡死）。

## 渲染验证（无头）

启动后看宿主机 `/tmp/aether-pc-share/pc-shot.ppm`（1280×720 PPM，2,764,800 字节）。
用 `python3` 扫描应出现荧光绿 `A`（约数千像素）与亮蓝胶囊；中心 logo 包围盒应居中 (~640,360)。
