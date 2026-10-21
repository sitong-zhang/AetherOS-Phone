# AetherOS 手机版 — 自包含终端 App（无需 Termux）

无需 root、不解锁 bootloader、不动系统分区。安装一个 APK，打开就是终端；
输入一条命令，完整的 AetherOS（真实 ARM64 Linux 6.6.70 内核 + 独立用户态：
init / 合成器 / SystemUI / 日志 / 崩溃 / 看门狗）就运行在这部"手机"里。

**它是一部手机，不是一个窗口**：竖屏 720×1600（1080×2400 手机的原生
9:20 比例），开机先是**锁屏**（大时钟 + 中文日期 + 电量 + 锁图标），点按
解锁进启动器；桌面是**实拍照片壁纸**，界面用 **iOS 风格液态玻璃**（背景
实时模糊、圆角边缘折射、镜面高光与染色）——由合成器以 CPU 软件渲染实现。

## 安装（像安装 DeepSeek 一样普通）

1. 下载 `aetheros-terminal.apk`（约 46MB：内嵌 QEMU 及全部依赖库），
   允许"安装未知来源应用"后安装——**不需要 Termux，不需要任何额外环境**。
2. 打开 App，它本身就是一个终端（黑底绿字 + 实时日志 + 命令行）。

> **版本提示**：安装时若出现「旧版本，可能无法正常运行」，说明装的是
> phone-v8 或更早的包（清单里没有声明 targetSdkVersion）。**phone-v11**
> 及以后已彻底消除该警告。
>
> **各版本修过的坑**（按发现顺序）：
>
> | 版本 | 症状 | 根因 |
> |---|---|---|
> | v9 | 一进去全黑屏 | 前端文件被打进 APK 根目录而非 `assets/`，AssetManager 读不到 |
> | v10 | `QEMU 退出 code=1` | 11 个库名带版本号（`libz.so.1`），Android 安装器只提取 `lib*.so` |
> | v11 | 点运行闪一下、状态「已安装·未启动」、一直连 5900 失败 | ①所有库的 `RUNPATH` 硬编码为 Termux 的 `/data/data/com.termux/files/usr/lib`，而 Android 的 linker **对普通应用忽略 `LD_LIBRARY_PATH`**，依赖一个都找不到，exec 即 exit 1；②APK 内 `.so` 只做了 4KB 页对齐（Android 15+ 要求 16KB）；③前端状态机把「QEMU 进程短暂存活」当成「系统就绪」，自动跳屏后无限重连，点「终端」两秒又被拉回 |
> | v12 前夜 | `cannot find "libdw.so.1" from verneed[5] in DT_NEEDED list` | Termux 打包残留：QEMU 的 `.gnu.version_r` 引用了 `libdw.so.1`/`libpulse.so`，但这两个库根本不在包里。glibc 容忍 verneed 与 DT_NEEDED 不一致，**bionic 不容忍**，直接拒绝加载 |
> | v12 | `cannot locate symbol "dwfl_standard_find_debuginfo"` | 版本表清了，但 bionic 会**急切解析全部 UND 符号**：Termux 编译的 QEMU 链接了 alsa/jack/pulse/libusb/spice/usbredir/ssh/curl 等可选功能库的符号，这些库都不在包里。这些功能我们全部用不到，但符号一个都不能缺 |
> | v13 | QEMU 起来了、状态「系统就绪」，但画面连上 46 字节后被断开 | 这条 QEMU 的 VNC 对**主动 SetPixelFormat** 支持有 bug：depth24 会直接断连、depth32 出不了帧。跳过协商、直接用 ServerInit 默认格式（32bpp LE R16G8B0，与渲染代码一致）即可 |

**v14 的修复**：不再发送 SetPixelFormat（ServerInit 默认格式与渲染解析完全
一致）；新增**崩溃监测器**——Java 层未捕获异常、前端 JS 错误/Promise 异常
分别落到终端日志、屏幕页提示与宿主日志（`POST /api/crash`），任何环节
出错都能直接截图定位；端到端测试升级为「完整 RFB 握手 + 收到
FramebufferUpdate」级别（模拟浏览器带掩码的 WS 帧），这类问题再也无法溜出构建。

## 使用：一条命令装系统

3. 在终端输入：

   ```sh
   aether install
   ```

   - 自动下载系统镜像（约 24MB，多镜像兜底，国内可装）并解包；
   - **下载完成后自动启动系统，画面直接显示在 App 里**——不需要任何
     额外操作，跟着引导走完就行。
4. 之后每次打开 App，系统自动恢复运行。

终端命令一览：`install` 安装 / `boot` 启动 / `stop` 停止 / `reboot` 重启 /
`status` 状态 / `update` 更新 / `reset` 重装 / `help` 帮助 / `clear` 清屏

## 系统更新：在系统里面点按钮，不用回终端

5. 在系统桌面打开「**系统更新**」应用（启动器网格里）：
   - 点「**检查更新**」——客机经内置网络通道访问宿主，查询 GitHub 上
     最新的 release，显示当前版本与最新版本；
   - 有新版本时点「**立即更新**」——自动停止当前系统、清除旧镜像、
     下载新版并重启，整个过程不用再碰终端。
6. 按 **Esc** 关闭窗口（外接键盘），或从启动器点其他应用切换。

> 更新通道原理：宿主把内嵌服务端口经内核参数 `aether_port=` 注入客机，
> 客机内应用经 QEMU user 网络访问 `10.0.2.2:<端口>` 回连宿主——客机内
> 的每次点击都走真实链路：compositor tap 转发 → 应用命中测试 → HTTP。

## 系统里有什么

- **锁屏**：大时钟、中文日期、真实电量（Android 宿主 HAL 上报时）
- **启动器**：应用网格，点按启动
- **应用**（全部经签名打包 + 沙箱运行，与系统服务同一条真实安装路径）：
  - Demo —— 原生图形演示
  - 设置 —— 真实系统读数面板（内核/内存/负载/服务/根分区）
  - 时钟 —— 大时钟 + 中文日期 + 秒进度条
  - **系统更新** —— 客机内 OTA（本条说明第 5 步）
- **可观测性**：序列化日志服务、崩溃管理器、软看门狗（开机 11 个服务）

## 技术形态（为什么不需要 Termux）

APK 内嵌 27 个 aarch64 原生库（QEMU 11.1.2 + 裁剪后的依赖闭包）+ Java
内嵌 HTTP/WebSocket 服务 + WebView 前端。所有库的 `RUNPATH` 均为 `$ORIGIN`，
依赖只在 nativeLibraryDir 内解析，不引用任何宿主机路径；APK 内未压缩的
`.so` 按 16KB 页对齐（Android 15/16 硬性要求）。

系统镜像（内核 + ext4 rootfs）按需下载到 App 私有目录，QEMU 从 5900 起
自动挑空闲端口启动，前端经 WebSocket 桥接 RFB 协议把画面呈现到 WebView。
屏幕页底部有一行状态诊断（就绪/启动中/未启动 · 端口 · 版本），
任何启动失败都会把 QEMU 自己 stderr 的最后 40 行打进日志并显示在屏幕上。

终端与系统画面在同一个 App 里：安装阶段看终端日志，装完自动切换到
系统画面；更新在系统内完成——这就是"装 APK 即得一部手机"的完整体验。
