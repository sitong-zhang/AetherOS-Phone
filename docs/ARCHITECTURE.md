# 架构：自包含 APK 内嵌虚拟机（当前主线）

## 与旧交付方式的对比

| 维度 | 旧方式（已归档至 `legacy/`） | 当前主线 |
| --- | --- | --- |
| 宿主 | Termux + 手工安装 `qemu-system-aarch64` | 自包含 Android APK（`dev.aether.vm`） |
| 显示 | 本地服务 + WebView / VNC 查看器 | APK 内 SDL2 图层直接上屏 |
| 镜像获取 | 用户运行脚本联网下载 tar.gz | kernel.img + system.qcow2 内置于 APK |
| 启动步骤 | 装 Termux → 装包 → 跑脚本 → 开查看器 → 连接 | 安装 APK → 点按一次开机 |
| 虚拟化引擎 | 外部 QEMU 二进制 | APK 内 `libqemu-system-aarch64.so` |
| 客机合成器 | 多进程窗口管理器 + DRM/KMS + SCM_RIGHTS | 单体状态机 + fbdev `/dev/fb0` + Unix socket IPC |
| 业务层 | 设置 / 时钟 / demo / hello（4 应用） | 阶段 1–5 桌面、灵动岛、商店、笔记、多任务、小窗、AOD |

两种方式运行的是同一个 AetherOS（同内核版本、同自研 init / 合成器骨架、同字体资产），差异集中在宿主承载与显示后端；业务层与系统盘构建为新增工作，不是对旧实现的 fork。

## 分层

```
Android APK
  ├── Java/JNI 层        BootActivity / VM 控制 / 崩溃与诊断覆盖层
  ├── 引擎层             SDL2 官方库 + libqemu-system-aarch64.so
  ├── 内置资产           assets/kernel.img, assets/system.qcow2, assets/vm.conf
  └── VM 运行时          vm/core/*（QEMU 参数组装、串口日志、ready 判定）
        ↓
客机 Linux 6.6.70 (aarch64, virt)
  ├── /sbin/aether-init    PID 1，服务监管，fd 0/1/2 → /dev/console
  ├── /etc/aether/run-compositor
  └── /sbin/aether-compositor   fbdev 软件渲染，/run/aetheros-comp.sock IPC
        ↓
ext4 系统盘（debugfs 注入 + e2fsck 一致性修复 + SHA-256 校验）
```

## 验证链

`vm/host/run_v6.sh` 启动全系统 QEMU TCG，客机执行盘内自检脚本，逐项输出状态断言（`SELFTEST_STAGE4_DONE` / `SELFTEST_STAGE5_DONE` / `SELFTEST_DONE`），并导出 PPM 帧转 PNG。宿主端检查 compositor 生命周期、崩溃码与 EXT4 错误后给出闭环判定。
