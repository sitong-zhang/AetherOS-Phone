#!/bin/bash
# build-aarch64.sh - 交叉编译安全引擎为 aarch64 静态二进制
#
# 产出：build/aarch64/secure/sbin/aether-os
# 与主机安全整机同源（secure/cli + secure/core），仅目标架构不同。
# 供 A 轨真 Linux 手机 OS 随开机运行设备自证自检；不依赖第三方密码库。
set -euo pipefail

SEC="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SEC/.." && pwd)"
CC="${AETHER_CC:-aarch64-linux-gnu-gcc}"
OUT="${AETHER_SECURE_OUT:-$REPO/build/aarch64/secure}"

command -v "$CC" >/dev/null 2>&1 || {
    echo "[secure-cross][ERROR] 缺交叉编译器: $CC" >&2; exit 1; }

mkdir -p "$OUT/sbin"
echo "[secure-cross] 工具链: $CC"
$CC -static -O2 -Wall -Wextra -Werror -I"$SEC/include" \
    "$SEC/cli/aether_os.c" "$SEC"/core/*.c \
    -o "$OUT/sbin/aether-os"
file "$OUT/sbin/aether-os" | sed 's/,.*//'
echo "[secure-cross] 完成 -> $OUT/sbin/aether-os"
