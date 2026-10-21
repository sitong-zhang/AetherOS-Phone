# AGENTS.md —— 接手者操作手册

> 本文件是 AetherOS-Phone 仓库的「操作手册」。读它的人要在最短时间内能**构建、启动、
> 测试、排查**这个系统。架构与原理请回到 [`README.md`](README.md) 与各份
> [`docs/`](docs/README.md) 文档；这里只讲「怎么做」和「坑在哪」。

---

## 0. 一句话定位

AetherOS = ARM64 Linux 6.6.70 内核 + 完全自有的用户态（init / 守护 / 合成器 / SystemUI /
包管理 / 沙箱 / 日志·崩溃·看门狗）。手机交付形为 Android App 内嵌 QEMU 跑客机。验证全部
基于 QEMU 真机级断言链，非模拟。

---

## 1. 环境与工具链

| 工具 | 版本（本沙箱已验证） | 用途 |
|---|---|---|
| `git-lfs` | 3.4.1 | 大二进制（内核/initramfs/.so/APK） |
| `aarch64-linux-gnu-gcc` | 13.3.0 | 交叉编译客机静态 ELF |
| `qemu-system-aarch64` | 8.2.2（Debian） | 跑真内核 + 客机 |
| `e2fsprogs`（mkfs.ext4） | — | `make_rootfs.sh` 打包 rootfs |
| `python3` | 3.x | 运行 `ci/boot-smoke.py` 等断言脚本 |
| `file` / `binutils`（readelf） | — | CI 断言 ARM64 静态 ELF |

一键安装：`bash bootstrap.sh`（Debian/Ubuntu 同族）。离线可加 `--no-lfs`。

**Git 认证**：本仓库推送到 `sitong-zhang/AetherOS-Phone`（public）。推送前确认 `gh auth status`
已登录且含 `repo` 权限；git credential helper 已配置即可。

---

## 2. 构建

### 2.1 交叉编译各组件

所有客机二进制都是**静态链接 aarch64 ELF**。通用范式：

```bash
aarch64-linux-gnu-gcc -static -O2 -Wall -o <out> <src>.c
```

关键点：

- **`aetherosd`** 需注入构建号：`-DAETHEROS_BUILD='"ci"'` 或 `"local"`。
- **`aether-compositor`** 是多文件编译：`aether-compositor.c aether-drm.c aether-render.c`，
  且需 `-I.`（用 `aether-drm.h` / `aether-render.h`）；CI 还加了 `-Wextra`。
- **字体头文件**：`aether-font.h`（8×16 ASCII）与 `aether-font-cjk.h`（16×16 CJK）由
  `tools/gen_font.py` / `tools/gen_cjk_font.py` 生成，已提交，通常无需重生成。
- **包管理签名**：`aether-pkg.c` 内嵌发布公钥 `aether-pkg-pubkey.h`（由 `aether-pkg keygen`
  生成，已提交）。`.aepkg` 用 `ci/test-signing.key`（**私钥，已被 .gitignore 排除**）签名。
- **其它组件**：`aether-systemui` `aether-settings` `aether-clock` `aether-updater`
  `aether-sandbox-run` `aether-logd` `aether-crashd` `aether-watchdog` `aether-ota`
  `aether-hal-android` 均单文件 `aarch64-linux-gnu-gcc -static` 编译。

`make_rootfs.sh` 通过环境变量定位这些二进制（`AETHER_INIT` / `AETHEROSD` / `AETHER_COMP` /
`AETHER_PKG` / `AETHER_SANDBOX_RUN` / `AETHER_SYSUI` / `AETHER_SETTINGS` / `AETHER_CLOCK` /
`AETHER_UPDATER` / `AETHER_LOGD` / `AETHER_CRASHD` / `AETHER_WATCHDOG` / `AETHER_OTA` /
`AETHER_HAL_ANDROID` 等，默认指向仓库根或 `build/`），缺失即 `FATAL` 退出。

### 2.2 构建 busybox（rootfs 基础）

```bash
ci/build-busybox.sh "$HOME/linux-build"     # 用版本化 ci/busybox-aarch64.config
# 产出 $HOME/linux-build/busybox-1.36.1/_install（BB 变量默认值指这里）
```

注意：busybox 1.36.1 在 Linux 6.8+ 头文件下需关闭 `CONFIG_TC`（CBQ qdisc 已移除），
配置已入库版本化，`oldconfig </dev/null` 保证零交互确定性。

### 2.3 构建 rootfs

```bash
./make_rootfs.sh          # 默认 ROOT=/tmp/linux-build/rootfs, IMG=/tmp/linux-build/aetheros-rootfs.ext4
```

它：复制 busybox 树 → 建标准目录 → 拷动态库 → 装 aether-init(PID1)+各服务 → 用发布密钥把
`demo/settings/clock/updater` 四个 `.aepkg` 签名打包进 `/data/aether/packages` → 生成
`/etc/aether/services` 服务清单 → `mkfs.ext4 -d` 导入成镜像。

> `aetheros-rootfs.ext4` **被 .gitignore 排除**（可由源码重建），不入库。

### 2.4 A/B 镜像组（OTA 验证用）

```bash
ci/make_ab.sh             # 生成 slotA/slotB/persist + ab-initramfs.cpio.gz
```

### 2.5 手机交付物

```bash
cd dist-phone && bash build-phone-tarball.sh          # aetheros-phone.tar.gz（kernel+rootfs）
cd dist-phone/terminal-apk && bash build-apk.sh        # aetheros-terminal.apk
cd dist-phone && bash build-phone-app-apk.sh           # aether-phone-app.apk（WebView 壳）
```

APK 构建需要 **Android SDK build-tools 35.0.0**（`zipalign` 必须支持 `-P 16`，Android 15+ 强制
16KB 页对齐）+ JDK（javac/d8，source/target 8）+ `platforms/android-34/android.jar`。

---

## 3. 运行与测试

### 3.1 启动客机（宿主机）

```bash
./boot.sh                 # 串口前台；Ctrl-A X 退出
# 默认 -M virt -cpu cortex-a57 -smp 2 -m 512M，virtio 磁盘+网络+9p share
```

手机场景用 `dist-phone/phone-boot.sh`（加 virtio-gpu/keyboard/tablet + VNC 127.0.0.1:5900）。

### 3.2 本地全链路 CI（推荐日常回归）

```bash
bash ci/run-ci.sh --skip-core     # 跳过需 Gradle 下载的 aether-core 段
# 产出 boot-smoke.summary.txt / ab-smoke.summary.txt / boot-smoke-virgl.summary.txt
```

`run-ci.sh` 在本地复演两条 GitHub Actions 工作流：交叉编译 → 工件断言 → IPC 契约 →
busybox → rootfs → boot-smoke（断言链）→ ab-smoke（A/B）。

### 3.3 GitHub Actions

| 工作流 | 触发 | 内容 |
|---|---|---|
| `.github/workflows/build.yml` | push / PR | 交叉编译 aether-init/aetherosd/compositor → `check-artifacts.sh`（ARM64 静态 ELF 双证据）→ `ipc-contract-smoke.sh`（宿主真 UDS + JSON 契约 13 项） |
| `.github/workflows/boot-smoke.yml` | push / PR / 每日 UTC 18:00 | busybox 交叉编译 → rootfs → `boot-smoke.py`（QEMU 启动 + 断言链） |

### 3.4 关键断言脚本

- `ci/check-artifacts.sh <elf...>`：断言为 aarch64 静态 ELF（`file` 无 dynamic section + `readelf` e_machine=AArch64）。
- `ci/ipc-contract-smoke.sh`：负例（无守护必须诚实失败）/ socket mode 0660 / JSON 字段 / uptime 活数据递增 / `-1` 状态文件 / 退出后 socket 清理。
- `ci/boot-smoke.py`：QEMU 启动断言链（PID1 身份 / 服务拓扑 / IPC / 9p / 图形栈 J 段 / 键盘 K 段 / 沙箱 S 段 / 崩溃自恢复 H / 优雅停机 I）。加 `--keyboard` 跑键盘段；加 `--gpu-virgl` 跑 virgl 检测段（无 render node 时 G 段如实 SKIP）。
- `ci/ab-smoke.py`：A/B 五段（首启 / 坏槽拒绝 / 切槽实启 / 恢复 / 优雅停机）。

---

## 4. 已知坑（接手必看，避免重踩）

1. **WsBridge 8 字节长度分支的 int 移位**（手机端，已修于 v18.1）：Java `int` 移位按 32 取模，
   `len >> 56` 实际 `len >> 24`，64KB 载荷写出垃圾长度。修复用 `long v = len & 0xFFFFFFFFL; v >>> ...`。
   详见 [`docs/PHONE-V18-ROOT-CAUSE.md`](docs/PHONE-V18-ROOT-CAUSE.md)。
2. **app.js 多矩形 FramebufferUpdate 解析**（已修）：矩形头读取抽为 `readRectHeader()`，
   按 `pendTotal/pendDone` 判断仍在同一条消息内，TCP 分片不再误入消息类型分发。
3. **aetherosd 的 `statvfs()` / `readdir(/proc)` 永久阻塞**（已修）：现只读常规小文件，
   必然返回；卷容量改用 `/proc/mounts` 类型。
4. **aetherosd `signal()` BSD 语义导致收不到 SIGTERM**（已修）：改用 `sigaction` 且 `sa_flags=0`。
5. **沙箱执行序**（已修）：必须先 `setresuid(1000)`（仍持 CAP_SETUID）再丢能力，否则 EPERM；
   seccomp 必须在身份切换之后安装；合成器 socket 须用**路径 chown**（`fchown` 对 unix socket 静默无效）。
6. **busybox suid applet 被 seccomp KILL**（已修）：rootfs 重建时关闭 `CONFIG_FEATURE_SUID` 等。
7. **DRM 常量抄错**（已修）：`DRM_PLANE_TYPE_*` 真实 enum 为 OVERLAY=0/PRIMARY=1/CURSOR=2。
8. **APK 16KB 页对齐**（已修）：`zipalign -P 16` 而非 `-p`；否则 Android 15+ linker 加载失败。
9. **APK 资源路径**：`assets/` 必须在 `$SRC` 下打包（条目为 `assets/index.html`），否则 WebView 黑屏。
10. **Android 只提取 `lib*.so`**：带版本后缀的库不会被 installd 提取，QEMU exec 时 CANNOT LINK。
11. **virgl 检测**：唯一诚实路径是 `DRM_IOCTL_VIRTGPU_GETPARAM(VIRTGPU_PARAM_3D_FEATURES)`；
    `DRM_CAP_3D` 是 libdrm 私有宏、`DRIVER_RENDER` 位恒置位，均不能判 3D 能力。
12. **rootfs 不可 `git clone` 即用**：`aetheros-rootfs.ext4` 被排除，需先交叉编译 + `make_rootfs.sh`。
    内核 / initramfs / .so / APK 走 LFS，记得 `git lfs pull`。

---

## 5. 当前进度与待办

### 5.1 已交付（详见 README.md 第四节与 docs/）

内核 + 总线/服务层、裸 KMS 自绘合成器、多进程窗口系统、CJK 字体、evdev 键盘、seccomp 沙箱、
A/B OTA、多 plane 枚举、virgl 能力检测、`.aepkg` 签名包、SystemUI、设置/时钟/更新应用、
手机终端 APK v18.1（重连三处根因已修）。

### 5.2 待办（明确的下一步）

- [ ] **内核侧硬化**：借鉴 GrapheneOS 的内存分配器硬化 / exploit mitigation（需重编内核配置）。
- [ ] **应用运行时推广沙箱**：目前仅 demo 带 `sandbox yes`，aetherosd/合成器/getty 仍全权限。
- [ ] **OTA 差分与签名校验**：A/B 两槽现为同一镜像副本，未实现差分包下载/写入非活跃槽。
- [ ] **多 plane 实际合成**：已枚举并上报 overlay/cursor=0，但合成器仍只用 primary plane blit。
- [ ] **virgl 真实 3D 路径**：已检测上报，未把绘制搬到 GPU 命令流；需具 render node 的宿主验证。
- [ ] **输入扩展**：接 virtio-mouse/tablet 指针输入；窗口焦点目前靠 Tab 键 / IPC 命令。
- [ ] **手势优先**：上滑回桌面、左右 peek（Sailfish 形态学），当前为点按式。
- [ ] **真机移植**：BSP / 设备树适配（内核与 rootfs 已分层，按 postmarketOS 模型留好位置）。
- [ ] **Content Hub**：应用经系统枢纽交换文件/文本（Ubuntu Touch 概念，未来才值得抄）。

---

## 6. 关键文件索引

| 我要做的事 | 看这里 |
|---|---|
| 理解整体架构 | `README.md`、[`docs/AETHEROS-OS-LINEAGE.md`](docs/AETHEROS-OS-LINEAGE.md) |
| 用户空间细节/证据 | [`docs/README-userland.md`](docs/README-userland.md) |
| 图形栈审计 | [`docs/GRAPHICS-STACK-AUDIT.md`](docs/GRAPHICS-STACK-AUDIT.md) |
| aetherosd IPC 契约 | [`docs/AETHEROSD-PROTOCOL.md`](docs/AETHEROSD-PROTOCOL.md) |
| 手机端根因/封存 | [`docs/PHONE-V18-ROOT-CAUSE.md`](docs/PHONE-V18-ROOT-CAUSE.md)、[`docs/PHONE-V18-ARCHIVE.md`](docs/PHONE-V18-ARCHIVE.md) |
| 改 PID1 / 服务监管 | `aether-init.c`、`make_rootfs.sh`（`/etc/aether/services`） |
| 改图形栈 | `aether-compositor.c` / `aether-drm.c` / `aether-render.c` / `aether-font*.h` |
| 改包管理/签名 | `aether-pkg.c`、`aether-pkg-pubkey.h`、`ci/test-signing.key`（排除）、`third_party/tweetnacl/` |
| 改 SystemUI | `aether-systemui.c` |
| 改沙箱 | `aether-sandbox.c` / `aether-sandbox.h` |
| 改手机 APK | `dist-phone/terminal-apk/src/`、`assets/app.js`、`build-apk.sh` |
| 跑测试/CI | `ci/run-ci.sh`、`ci/boot-smoke.py`、`ci/ab-smoke.py`、`.github/workflows/` |
| 重建 rootfs | `make_rootfs.sh` |
| 重建镜像包/APK | `dist-phone/build-phone-tarball.sh`、`dist-phone/terminal-apk/build-apk.sh` |

---

## 7. 代码约定

- **语言**：客机核心为 C（C11 风格，静态链接，零第三方运行时依赖）；手机前端为 Java（APK）+ JS（WebView）。
- **构建标志**：`aetherosd` 必须 `-DAETHEROS_BUILD=...`；合成器必须多文件 + `-I.` + 严格警告
  （`-Wall -Wextra`，要求零警告）。
- **诚实原则（贯穿全仓库）**：读不到的字段一律 `unknown` / `-1` / `false`，绝不编造；
  断言期望值全部取自真实取证日志，不为通过而放宽。
- **IPC 模型**：UDS + 单条消息协议（`AetherBus`）；合成器为「一连接一请求一应答即 close」的
  短连接，客户端须每命令独立连接。
- **字体**：构建期 bake 进 `.h`，guest 零字体依赖；CJK 为精选子集，缺字显 tofu 方框。
- **提交信息**：建议按 `[组件] 动作：简述` 风格（如 `[compositor] fix: DRM plane type enum`）。
- **文档同步**：改了行为务必同步对应 `docs/` 文档的「踩坑记录」与「诚实边界」段。
