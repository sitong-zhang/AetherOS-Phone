#!/bin/bash
# build-busybox-pc.sh - 构建静态 busybox（x86_64 根文件系统基座）
# 若项目内已存在种子（build-pc/busybox/_install）则直接复用，避免重复下载。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BB="$ROOT/build-pc/busybox"

if [ -x "$BB/_install/bin/busybox" ]; then
    echo "[busybox] 已存在静态 busybox，跳过构建: $BB/_install/bin/busybox"
    exit 0
fi

WORK=/tmp/aether-pc-busybox-build
mkdir -p "$WORK"
cd "$WORK"
if [ ! -f busybox-1.36.1.tar.bz2 ]; then
    echo "[busybox] 下载 busybox 1.36.1 ..."
    wget -q https://busybox.net/downloads/busybox-1.36.1.tar.bz2
fi
rm -rf busybox-1.36.1
tar xf busybox-1.36.1.tar.bz2
cd busybox-1.36.1
make defconfig >/dev/null 2>&1
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' .config
echo "[busybox] 编译（静态）..."
make -j"$(nproc)" >/dev/null 2>&1
make CONFIG_PREFIX="$BB/_install" install >/dev/null 2>&1
echo "[busybox] 完成 -> $BB/_install/bin/busybox"
file "$BB/_install/bin/busybox"
