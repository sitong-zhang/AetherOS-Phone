# CLAUDE.md

AetherOS Phone 客机用户态工程指引（简要版，详见 `AGENTS.md`）。

本目录是内置于 Android APK 的手机操作系统客机用户态：aarch64 Linux 6.6.70 内核
（由 `aether-vm` 工程提供）+ 纯 C 静态链接用户态（`aether-init` / `aetherosd` /
`aether-compositor` / `aether-watchdog` / `aether-phone.c`）。项目只存在手机形态，
PC 桌面方案已废弃，界面内不绘制任何鼠标指针，输入只识别触摸屏与键盘。

- 构建：`bash build-aarch64.sh`（aarch64 真机构建）；`bash build-host.sh` 仅做 host
  原生编译验证，不产出可启动镜像。
- 系统盘与真机闭环：`/workspace/aether-vm/make_system_v6.sh` →
  `/workspace/aether-vm/host/run_v6.sh`（QEMU TCG 开机 + 逐帧取证）。
- 关键约束：平面类型属性名小写 `type`；`/run/aether` 须在 tmpfs /run 挂载后创建；
  合成器 `poll` 数组按实际 fd 数分配且渲染后必须回到 `poll`；改完源码必须用
  `AETHER_BUILD=.../root-full` 重编，否则系统盘注入旧二进制。
- 玻璃材质：上游源码副本与逐条映射见 `third_party/liquid-glass/`，固化常量见
  `src/aether-glass-spec.h`。
- 详细工程指引见 `AGENTS.md`，项目说明见 `README.md`。
