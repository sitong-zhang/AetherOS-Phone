#!/bin/bash
# build-aarch64.sh - 静态交叉编译 AetherOS 用户态（ARM64 / aarch64）
# 目标：在 ARM64 真机 / Limbo(Android) 上运行的手机操作系统用户态。
# 显示后端仅 fbdev（真实 Limbo 走 /dev/fb0），不链接 libdrm（aarch64 无静态 libdrm）。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
SRC="$ROOT/src"
CC="${AETHER_CC:-aarch64-linux-gnu-gcc}"
OUT="${AETHER_BUILD:-$REPO/build/aarch64/root}"
mkdir -p "$OUT/sbin" "$OUT/bin"

# -DAETHER_NO_DRM: 不编译 DRM 路径，仅 fbdev；规避 aarch64 缺静态 libdrm。
# 不引用 /usr/include/libdrm（该头文件在 aarch64 交叉环境不存在）。
# 注意：不显式加 -pthread。本工程用户态二进制均为单线程（仅 aether-init 用 fork()），
# 且 glibc 2.34+ 已把 pthread 并入 libc，静态链接无需再拉 libpthread.a。
# -rdynamic 保留，便于崩溃时回溯符号可读。
FLAGS="-static -O2 -Wall -Wextra -I$SRC -DAETHER_NO_DRM -rdynamic"

# 外壳：本项目只存在手机形态（aether-phone.c），PC 桌面方案已彻底废弃。
SHELL_SRC=aether-phone.c

echo "[build-aarch64] 工具链: $CC"
echo "[build-aarch64] aether-init"
$CC $FLAGS -o "$OUT/sbin/aether-init" "$SRC/aether-init.c" "$SRC/aether-log.c"

echo "[build-aarch64] aetherosd"
$CC $FLAGS -o "$OUT/sbin/aetherosd" "$SRC/aetherosd.c" "$SRC/aether-log.c"


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
    ( cd "$ROOT/assets" && aarch64-linux-gnu-objcopy -I binary -O elf64-littleaarch64 -B aarch64 \
        aether-font.ttf "$VFONT_OBJ" )
    VFONT_SRC="$SRC/aether-vfont.c"
    VFONT_DEF="-DAETHER_VECTOR_FONT"
    echo "[build] 矢量字体已内嵌 ($(stat -c %s "$FONT_TTF") B, 7720 码点)"
else
    echo "[build] 未找到 assets/aether-font.ttf，回退 1001 字点阵后端"
fi
echo "[build-aarch64] aether-compositor (+display +render +icons +$SHELL_SRC +ui +apps) [NO_DRM/fbdev]"
$CC $FLAGS -o "$OUT/sbin/aether-compositor" \
    "$SRC/aether-compositor.c" "$SRC/aether-display.c" \
    "$SRC/aether-android-bridge.c" \
    "$SRC/aether-android-util.c" \
    "$SRC/aether-render.c" "$SRC/aether-anim.c" "$SRC/aether-glass.c" "$SRC/aether-material.c" "$SRC/aether-icons.c" "$SRC/$SHELL_SRC" "$SRC/aether-ui.c" \
    "$SRC/aether-apps.c" "$SRC/aether-apps2.c" "$SRC/aether-settings.c" \
    "$SRC/aether-island.c" "$SRC/aether-discover.c" "$SRC/aether-store.c" "$SRC/aether-notes.c" "$SRC/aether-log.c" \
    $VFONT_DEF $VFONT_SRC $VFONT_OBJ -Wl,-z,noexecstack -lm

echo "[build-aarch64] aether-watchdog"
$CC $FLAGS -o "$OUT/sbin/aether-watchdog" "$SRC/aether-watchdog.c" "$SRC/aether-log.c"
# 安卓兼容层服务：独立二进制，由 init.d 声明拉起；缺依赖时如实降级
$CC $FLAGS -o "$OUT/sbin/aether-android" "$SRC/aether-android.c" "$SRC/aether-android-util.c" "$SRC/aether-log.c"


echo "[build-aarch64] aether-sysinfo / aether-hello / aether-shot / aether-ipc"
$CC $FLAGS -o "$OUT/bin/aether-sysinfo" "$SRC/aether-sysinfo.c"
$CC $FLAGS -o "$OUT/bin/aether-hello" "$SRC/aether-hello.c"
$CC $FLAGS -o "$OUT/bin/aether-shot" "$SRC/aether-shot.c"
$CC $FLAGS -o "$OUT/bin/aether-ipc" "$SRC/aether-ipc.c"

echo "[build-aarch64] file 校验（应为 ARM aarch64 静态）"
file "$OUT/sbin/aether-compositor" | sed 's/,.*//'
echo "[build-aarch64] 完成 -> $OUT"
ls -la "$OUT/sbin" "$OUT/bin"
