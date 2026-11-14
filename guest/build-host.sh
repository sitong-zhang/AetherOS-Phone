#!/bin/bash
# build-host.sh - 静态编译 AetherOS 用户态（host x86_64 原生，用于取证验证）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
SRC="$ROOT/src"
# 构建产物统一放到 build/（由 .gitignore 排除），保持工作区整洁
OUT="${AETHER_BUILD:-$REPO/build/host/root}"
mkdir -p "$OUT/sbin" "$OUT/bin"

FLAGS="-static -O2 -Wall -Wextra -I$SRC -I/usr/include/libdrm -rdynamic -pthread"

echo "[build-host] aether-init"
gcc $FLAGS -o "$OUT/sbin/aether-init" "$SRC/aether-init.c" "$SRC/aether-log.c"

echo "[build-host] aetherosd"
gcc $FLAGS -o "$OUT/sbin/aetherosd" "$SRC/aetherosd.c" "$SRC/aether-log.c"

# 外壳：本项目只存在手机形态（aether-phone.c）。PC 桌面方案已彻底废弃，
# 不再提供 desktop 构建入口，也不保留 aether-desktop.c 实现。
SHELL_SRC=aether-phone.c


# ---- 矢量字体 blob ----
# assets/aether-font.ttf 是 Noto Sans CJK SC 的 TrueType 子集（tools/gen_vector_font.py
# 生成）。用 objcopy 转为目标文件静态链入合成器，运行时零外部文件依赖，也避开
# 向 ext4 系统盘注入大文件带来的 journal/位图一致性风险。
# 在 assets 目录内以裸文件名生成，符号固定为
# _binary_aether_font_ttf_start / _binary_aether_font_ttf_end。
FONT_TTF="$ROOT/assets/aether-font.ttf"
VFONT_OBJ=""
VFONT_SRC=""
VFONT_DEF=""
if [ -f "$FONT_TTF" ]; then
    VFONT_OBJ="$OUT/aether-font-blob.o"
    ( cd "$ROOT/assets" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        aether-font.ttf "$VFONT_OBJ" )
    VFONT_SRC="$SRC/aether-vfont.c"
    VFONT_DEF="-DAETHER_VECTOR_FONT"
    echo "[build] 矢量字体已内嵌 ($(stat -c %s "$FONT_TTF") B, 7720 码点)"
else
    echo "[build] 未找到 assets/aether-font.ttf，回退 1001 字点阵后端"
fi
echo "[build-host] aether-compositor (+display +drm +render +icons +$SHELL_SRC +ui +apps)"
gcc $FLAGS -o "$OUT/sbin/aether-compositor" \
    "$SRC/aether-compositor.c" "$SRC/aether-display.c" "$SRC/aether-drm.c" \
    "$SRC/aether-android-bridge.c" \
    "$SRC/aether-android-util.c" \
    "$SRC/aether-render.c" "$SRC/aether-anim.c" "$SRC/aether-glass.c" "$SRC/aether-material.c" "$SRC/aether-icons.c" "$SRC/$SHELL_SRC" "$SRC/aether-ui.c" \
    "$SRC/aether-apps.c" "$SRC/aether-apps2.c" "$SRC/aether-settings.c" \
    "$SRC/aether-island.c" "$SRC/aether-discover.c" "$SRC/aether-store.c" "$SRC/aether-notes.c" "$SRC/aether-log.c" \
    $VFONT_DEF $VFONT_SRC $VFONT_OBJ -Wl,-z,noexecstack -ldrm -lm

echo "[build-host] aether-watchdog"
gcc $FLAGS -o "$OUT/sbin/aether-watchdog" "$SRC/aether-watchdog.c" "$SRC/aether-log.c"
echo "[build-host] aether-android (安卓兼容层服务)"
gcc $FLAGS -o "$OUT/sbin/aether-android" "$SRC/aether-android.c" "$SRC/aether-android-util.c" "$SRC/aether-log.c"

echo "[build-host] aether-sysinfo / aether-hello / aether-shot / aether-ipc"
gcc $FLAGS -o "$OUT/bin/aether-sysinfo" "$SRC/aether-sysinfo.c"
gcc $FLAGS -o "$OUT/bin/aether-hello" "$SRC/aether-hello.c"
gcc $FLAGS -o "$OUT/bin/aether-shot" "$SRC/aether-shot.c"
gcc $FLAGS -o "$OUT/bin/aether-ipc" "$SRC/aether-ipc.c"

echo "[build-host] 完成 -> $OUT"
ls -la "$OUT/sbin" "$OUT/bin"
