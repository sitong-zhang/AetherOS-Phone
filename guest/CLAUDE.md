# CLAUDE.md

AetherOS（电脑版）是一个从零构建的 x86_64 电脑操作系统：Linux 6.6.70 内核 + 完全自有的用户态
（aether-init / aetherosd / aether-compositor / aether-watchdog），可在 QEMU 中以可启动 ext4 镜像运行，
桌面渲染出品牌 logo。它与手机版是**相互独立**的工程，仅共用名字 AetherOS 与图标。

- 构建：`bash build-pc.sh && bash build-busybox-pc.sh && bash make_rootfs_pc.sh && bash boot-pc.sh`
- 验证渲染：宿主机 `/tmp/aether-pc-share/pc-shot.ppm`（由 pcshot 经 9p 导出的首屏截图）
- 关键约束：平面类型属性名为小写 `type`；`/run/aether` 须在 tmpfs /run 挂载后创建；
  合成器 poll 数组按实际 fd 数分配且渲染后必须回到 poll。
- 详细工程指引见 `AGENTS.md`，项目说明见 `README.md`。
