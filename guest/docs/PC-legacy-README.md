# AetherOS（电脑版 / PC）

AetherOS 是一个**从零构建的 x86_64 电脑操作系统**：`Linux 6.6.70` 内核 + 完全自有的用户态，
由自研的 `aether-init` 服务管理器、系统守护 `aetherosd`、DRM/KMS 合成器
`aether-compositor`、健康看门狗 `aether-watchdog` 等组成。可在 QEMU 中以可启动镜像形式运行。

> **与手机版的关系**：AetherOS 同时有「手机版」与「电脑版」两个**相互独立**的项目。
> 两者**共用同一个名字 AetherOS 与同一个图标**（深底 + 荧光绿 `A` + 亮蓝胶囊横条），
> 但代码互不复用——电脑版不依赖手机版的任何代码，是真正的从零搭建。
> 手机版位于另一个独立仓库（原 `aether-linux`），本目录是电脑版的专属工程。

## 它是什么

- **内核**：标准 `Linux 6.6.70 (x86_64)` 内核（bzImage），通用 PC 内核，非手机专属代码。
- **用户态（自有）**：全部用 C 从零编写，静态链接，无 Android / HAL / aarch64 假设。
  - `aether-init`：PID 1，挂载基础文件系统、按依赖启动并守护服务、回收僵尸进程。
  - `aetherosd`：系统守护，经 Unix 域套接字 `/run/aetheros.sock` 暴露 AetherBus（实时 CPU/内存/运行时长/服务状态）。
  - `aether-compositor`：DRM/KMS 原子提交驱动 virtio-gpu，软件合成桌面，支持键鼠/平板输入，经 `/run/aetheros-comp.sock` 提供 `status` / `screenshot` / `ping` IPC。
  - `aether-watchdog`：健康看门狗，对关键服务做心跳探测，仅在连续失活后才杀进程交由 init 重启。
  - 工具：`aether-sysinfo`（查询守护）、`aether-shot`（截图客户端）、`aether-hello`。
- **桌面**：深色渐变壁纸 + 顶栏（时钟 / CPU / 内存）+ 居中 **AetherOS logo（程序化重绘）+ AetherOS 文字**；鼠标移动、点击开/关演示窗口。

## 目录结构

```
aether-pc/
├── aetheros-kernel-6.6.70-x86_64.bzImage   # 客机内核
├── build-pc.sh                            # 静态编译全部用户态
├── build-busybox-pc.sh                    # 构建静态 busybox（根文件系统基座）
├── make_rootfs_pc.sh                      # 生成 ext4 根文件系统镜像
├── boot-pc.sh                             # 在 QEMU 中启动（--gui 开图形窗口）
├── src/                                   # 全部自有 C 源码
│   ├── aether-init.c  aetherosd.c  aether-compositor.c
│   ├── aether-drm.c/h  aether-render.c/h  aether-watchdog.c
│   ├── aether-log.c/h  aether-sysinfo.c  aether-shot.c  aether-hello.c
│   └── aether-font.h                      # 内嵌 8x16 位图字体（数据资产）
├── build-pc/                              # 构建产物（二进制 / busybox / stage）
└── docs/                                  # 设计文档
```

## 构建与运行

```bash
# 1) 编译用户态（静态，需 libdrm 开发头文件）
bash build-pc.sh
# 2) 准备静态 busybox（若 build-pc/busybox 已有种子则跳过下载）
bash build-busybox-pc.sh
# 3) 生成 ext4 根文件系统镜像 -> /tmp/aether-pc-build/aetheros-rootfs-pc.ext4
bash make_rootfs_pc.sh
# 4) 启动（默认无头：串口日志；--gui 打开图形窗口）
bash boot-pc.sh            # 无头
bash boot-pc.sh --gui      # 图形窗口
```

启动后在宿主机 `/tmp/aether-pc-share/` 可拿到客机首屏截图 `pc-shot.ppm`（由 `pcshot` 服务经 9p 共享导出），
用图片查看器或 `python3` 打开即可看到 AetherOS 桌面与居中 logo。

## 验证渲染（无头也能核验）

无需图形界面即可确认桌面真实渲染（非黑屏）：

```python
# 截图 2,764,800 字节 = 1280*720*3；扫描应出现荧光绿(A)与亮蓝(胶囊)像素
python3 - <<'PY'
data=open("/tmp/aether-pc-share/pc-shot.ppm","rb").read()
assert data[:2]==b"P6"; w,h=map(int,data[3:].split(b"\n")[0].split())
print("ok", w, h, len(data))   # 1280 720 2764800
PY
```

## 已知约束

- 当前在沙箱中以 QEMU `TCG`（纯软件）运行，无 `/dev/kvm`；启动与渲染均正常，只是速度偏慢。
- 显示设备固定为 `virtio-gpu-pci` + 1280×720；合成器支持 `AETHER_RES=WxH` 覆盖分辨率。
- 输入设备通过 `virtio-keyboard/tablet/mouse` 暴露，合成器自动识别 EV_KEY / EV_REL / EV_ABS。
