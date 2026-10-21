# docs/ —— 工程文档索引

> 本目录收纳 AetherOS-Phone 的工程文档。建议按下面的「阅读顺序」吸收，可在一小时内建立
> 完整心智模型。架构与操作请以仓库根目录的 [`README.md`](../README.md) 与
> [`AGENTS.md`](../AGENTS.md) 为准。

## 阅读顺序

1. **[`AETHEROS-OS-LINEAGE.md`](AETHEROS-OS-LINEAGE.md)** —— 先定性「AetherOS 属于哪一类」。
   逐项对照 AOSP / postmarketOS / Ubuntu Touch / OpenHarmony 等，写明借了什么、明确不抄什么、
   以及诚实边界（无基带 / 无 APK 兼容 / 软件渲染 / 单设备）。
2. **[`README-userland.md`](README-userland.md)** —— 用户空间各阶段的落地细节、实跑证据、
   踩坑记录、诚实边界。最厚的一份，按需查阅（Phase A → P0-6 → P1-1x → P1-2x → P2-11 → P3-14 → P4-1/P5）。
3. **[`GRAPHICS-STACK-AUDIT.md`](GRAPHICS-STACK-AUDIT.md)** —— 图形栈工程审计：ioctl 契约、
   已修缺陷、验证证据、诚实边界。
4. **[`AETHEROSD-PROTOCOL.md`](AETHEROSD-PROTOCOL.md)** —— `aetherosd` 的 IPC 契约
   （`aetherosd.state/1`），对接实时状态时的规范来源；含访问控制与传输层（AetherBus → UDS）说明。
5. **[`PHONE-V18-ROOT-CAUSE.md`](PHONE-V18-ROOT-CAUSE.md)** —— phone-v18 终端 APK「一直重连」的
   三处根因定位与复现（WsBridge encode 移位 / app.js 多矩形解析 / SSE 重复推送）。
6. **[`PHONE-V18-ARCHIVE.md`](PHONE-V18-ARCHIVE.md)** —— phone-v18.1 封存说明：交付物清单、
   相对 v18 首发的修改、重建方式、验证记录、已知边界。
7. **[`SOURCE-README.md`](SOURCE-README.md)** —— 历史：Android 兼容层调试上下文（Weston/Wayland
   路径），已被 P0-6 的裸 KMS 自绘合成器取代，仅作来龙去脉存档。
8. **[`README-extension.md`](README-extension.md)** —— 历史：真 Linux 内核子系统扩展记录
   （virtio/9p/SMP 等），已被后续阶段吸收，仅作来龙去脉存档。
9. **[`INSTALL-phone-v7.md`](INSTALL-phone-v7.md)** —— 用户面向的安装引导（v7 版），供对照手机端
   交付形态演进。

## 文档与代码的对应

| 文档 | 主要对应代码 |
|---|---|
| `README-userland.md` | `aether-init.c` / `aetherosd.c` / `aether-compositor.c` / `make_rootfs.sh` |
| `GRAPHICS-STACK-AUDIT.md` | `aether-drm.c` / `aether-render.c` / `aether-compositor.c` / `aether-font*.h` |
| `AETHEROSD-PROTOCOL.md` | `aetherosd.c` + 宿主 Kotlin 客户端（历史验证） |
| `PHONE-V18-ROOT-CAUSE.md` | `dist-phone/terminal-apk/src/` + `assets/app.js` |
| `AETHEROS-OS-LINEAGE.md` | 全仓库组件对照 |

## 约定

- 所有文档坚持「诚实原则」：读不到的值标 `unknown` / `-1` / `false`，不编造；
  断言期望值取自真实取证日志，不为通过而放宽。
- 改了行为务必同步对应文档的「踩坑记录」与「诚实边界」段。
