# guest/ —— AetherOS 客机用户态源码

本目录是 AetherOS 客机系统的用户态源码与构建脚本。产物为静态链接的 aarch64 ELF，
经 `vm/make_system_v6.sh` 注入 ext4 系统盘后，由自包含 APK 内的 QEMU 虚拟机启动。

## 内容

| 路径 | 说明 |
| --- | --- |
| `src/` | 合成器、init、显示后端、渲染、字体、图标、动画、桌面、阶段 1–5 业务层 |
| `tools/` | 图标离线光栅化、帧取证、state 断言、字体生成脚本 |
| `assets/` | 图标原始 SVG（Lucide）等来源文件 |
| `THIRD_PARTY/` | 第三方归属与许可 |
| `build-aarch64.sh` | 交叉编译静态 aarch64 ELF，`AETHER_BUILD` 可重定向输出 |
| `build-pc.sh` | x86_64 host 构建，用于桌面取证与 56 帧证据 |
| `make_rootfs_*.sh` | 各平台 rootfs 打包 |

## 两条显示后端

- 取证/桌面：PC 路径，DRM/KMS 或离屏缓冲，供 `tools/capture_stage5.py` 出帧。
- 真机客机：`AETHER_BACKEND=fbdev`，直接写 `/dev/fb0`，由 init 启动 `/etc/aether/run-compositor`。

## 历史说明

本目录早期以「AetherOS 电脑版」立项，旧文档曾表述为与手机版相互独立、代码互不复用；
该表述已不成立。当前 `guest/` 即为主线客机源码，与 `vm/` 共同构成一套系统。
旧 PC 版 README 保留于 `docs/PC-legacy-README.md` 以便追溯。

## 状态文档

`AetherOS-系统进度说明.md` 与 `OPENSOURCE-ATTRIBUTION.md` 为阶段 5 时的进度与归属快照；
最新主线状态以仓库根 `README.md` 与 `docs/ARCHITECTURE.md` 为准。
