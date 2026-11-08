#!/bin/bash
# build-aarch64.sh - 静态交叉编译 AetherOS 用户态（ARM64 / aarch64）
# 目标：在 ARM64 真机 / Limbo(Android) 上运行的手机操作系统用户态。
# 显示后端仅 fbdev（真实 Limbo 走 /dev/fb0），不链接 libdrm（aarch64 无静态 libdrm）。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
CC="${AETHER_CC:-aarch64-linux-gnu-gcc}"
OUT="${AETHER_BUILD:-/root/aether-archive/aether-aarch64-build/root-min}"
mkdir -p "$OUT/sbin" "$OUT/bin"

# -DAETHER_NO_DRM -DAETHER_MINIMAL: 不编译 DRM 路径，仅 fbdev；规避 aarch64 缺静态 libdrm。
# 不引用 /usr/include/libdrm（该头文件在 aarch64 交叉环境不存在）。
FLAGS="-static -O2 -Wall -Wextra -I$SRC -DAETHER_NO_DRM -DAETHER_MINIMAL -rdynamic -pthread"

# 外壳：phone（竖屏手机操作系统，默认）
SHELL_SRC="${AETHER_SHELL:-phone}"
if [ "$SHELL_SRC" = "desktop" ]; then SHELL_SRC=aether-desktop.c; else SHELL_SRC=aether-phone.c; fi

echo "[build-aarch64] 工具链: $CC"
echo "[build-aarch64] aether-init"
$CC $FLAGS -o "$OUT/sbin/aether-init" "$SRC/aether-init.c" "$SRC/aether-log.c"

echo "[build-aarch64] aetherosd"
$CC $FLAGS -o "$OUT/sbin/aetherosd" "$SRC/aetherosd.c" "$SRC/aether-log.c"

echo "[build-aarch64] aether-compositor (+display +render +icons +$SHELL_SRC +ui +apps) [NO_DRM/fbdev]"
$CC $FLAGS -o "$OUT/sbin/aether-compositor" \
    "$SRC/aether-compositor.c" "$SRC/aether-display.c" \
    "$SRC/aether-render.c" "$SRC/aether-anim.c" "$SRC/aether-glass.c" "$SRC/aether-icons.c" "$SRC/$SHELL_SRC" "$SRC/aether-ui.c" \
    "$SRC/aether-apps.c" "$SRC/aether-apps2.c" "$SRC/aether-settings.c" \
    "$SRC/aether-island.c" "$SRC/aether-discover.c" "$SRC/aether-store.c" "$SRC/aether-notes.c" "$SRC/aether-log.c" \
    -lm

echo "[build-aarch64] aether-watchdog"
$CC $FLAGS -o "$OUT/sbin/aether-watchdog" "$SRC/aether-watchdog.c" "$SRC/aether-log.c"

echo "[build-aarch64] aether-sysinfo / aether-hello / aether-shot / aether-ipc"
$CC $FLAGS -o "$OUT/bin/aether-sysinfo" "$SRC/aether-sysinfo.c"
$CC $FLAGS -o "$OUT/bin/aether-hello" "$SRC/aether-hello.c"
$CC $FLAGS -o "$OUT/bin/aether-shot" "$SRC/aether-shot.c"
$CC $FLAGS -o "$OUT/bin/aether-ipc" "$SRC/aether-ipc.c"

echo "[build-aarch64] file 校验（应为 ARM aarch64 静态）"
file "$OUT/sbin/aether-compositor" | sed 's/,.*//'
echo "[build-aarch64] 完成 -> $OUT"
ls -la "$OUT/sbin" "$OUT/bin"
