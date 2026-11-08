#!/bin/bash
# build-pc.sh - 静态编译 AetherOS PC 用户态（x86_64 原生）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
# 构建产物统一放到其他存储（/root/aether-archive），不污染工作区
OUT="${AETHER_BUILD:-/root/aether-archive/aether-pc-build/root}"
mkdir -p "$OUT/sbin" "$OUT/bin"

FLAGS="-static -O2 -Wall -Wextra -I$SRC -I/usr/include/libdrm -rdynamic -pthread"

echo "[build-pc] aether-init"
gcc $FLAGS -o "$OUT/sbin/aether-init" "$SRC/aether-init.c" "$SRC/aether-log.c"

echo "[build-pc] aetherosd"
gcc $FLAGS -o "$OUT/sbin/aetherosd" "$SRC/aetherosd.c" "$SRC/aether-log.c"

# 外壳：phone（默认，竖屏手机操作系统）或 desktop（PC 桌面，备用）
SHELL_SRC="${AETHER_SHELL:-phone}"
if [ "$SHELL_SRC" = "desktop" ]; then SHELL_SRC=aether-desktop.c; else SHELL_SRC=aether-phone.c; fi

echo "[build-pc] aether-compositor (+display +drm +render +icons +$SHELL_SRC +ui +apps)"
gcc $FLAGS -o "$OUT/sbin/aether-compositor" \
    "$SRC/aether-compositor.c" "$SRC/aether-display.c" "$SRC/aether-drm.c" \
    "$SRC/aether-render.c" "$SRC/aether-anim.c" "$SRC/aether-glass.c" "$SRC/aether-icons.c" "$SRC/$SHELL_SRC" "$SRC/aether-ui.c" \
    "$SRC/aether-apps.c" "$SRC/aether-apps2.c" "$SRC/aether-settings.c" \
    "$SRC/aether-island.c" "$SRC/aether-discover.c" "$SRC/aether-store.c" "$SRC/aether-notes.c" "$SRC/aether-log.c" \
    -ldrm -lm

echo "[build-pc] aether-watchdog"
gcc $FLAGS -o "$OUT/sbin/aether-watchdog" "$SRC/aether-watchdog.c" "$SRC/aether-log.c"

echo "[build-pc] aether-sysinfo / aether-hello / aether-shot / aether-ipc"
gcc $FLAGS -o "$OUT/bin/aether-sysinfo" "$SRC/aether-sysinfo.c"
gcc $FLAGS -o "$OUT/bin/aether-hello" "$SRC/aether-hello.c"
gcc $FLAGS -o "$OUT/bin/aether-shot" "$SRC/aether-shot.c"
gcc $FLAGS -o "$OUT/bin/aether-ipc" "$SRC/aether-ipc.c"

echo "[build-pc] 完成 -> $OUT"
ls -la "$OUT/sbin" "$OUT/bin"
