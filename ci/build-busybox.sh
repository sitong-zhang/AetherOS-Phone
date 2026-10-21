#!/bin/bash
# build-busybox.sh —— 交叉编译 busybox 并产出 _install 树（CI 与本地沙箱共用）
#
# 用法: build-busybox.sh [工作目录]   默认 /tmp/linux-build
# 产物: <工作目录>/busybox-1.36.1/_install（make_rootfs.sh 的 BB 输入）
#
# 配置来源（按优先级）：
#   1) 本脚本同目录的 busybox-aarch64.config（已知良好配置，随仓库版本化）
#   2) 不存在时回退 defconfig + 关闭 CONFIG_TC 的兜底逻辑
#
# 为什么关 TC（2026-10-18 实踩）：busybox 1.36.1 的 networking/tc.c 引用
# TCA_CBQ_* 符号，而 Linux 6.8 的 UAPI 头文件（linux-libc-dev 6.8）已随
# CBQ qdisc 删除这些定义——默认配置在当前工具链下必然编译失败。
# AetherOS rootfs 不使用 tc applet，关闭它不影响任何既定功能。
#
# 幂等：源码树已存在则跳过下载；配置与构建均为增量。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-/tmp/linux-build}"
BB_VER=1.36.1
SRC="$WORK/busybox-$BB_VER"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
KNOWN_GOOD="$SCRIPT_DIR/busybox-aarch64.config"

mkdir -p "$WORK"
if [ ! -d "$SRC" ]; then
  echo "[busybox] 下载 busybox-$BB_VER 源码 (~2.6MB) ..."
  curl -fsSL --retry 3 -o "$WORK/busybox.tar.bz2" \
    "https://busybox.net/downloads/busybox-1.36.1.tar.bz2"
  tar -xjf "$WORK/busybox.tar.bz2" -C "$WORK"
fi

if [ -f "$KNOWN_GOOD" ]; then
  # 已知良好配置：与 busybox 1.36.1 精确匹配，oldconfig 零新符号、零交互
  echo "[busybox] 使用版本化已知良好配置: $KNOWN_GOOD"
  cp "$KNOWN_GOOD" "$SRC/.config"
  # stdin 接 /dev/null：即使将来出现新符号也确定性地取默认值，绝不挂起等输入
  make -C "$SRC" oldconfig </dev/null >/dev/null
else
  echo "[busybox] 未找到已知良好配置，回退: defconfig + 关闭 CONFIG_TC"
  make -C "$SRC" defconfig </dev/null >/dev/null
  sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/; s/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' "$SRC/.config"
  make -C "$SRC" oldconfig </dev/null >/dev/null
fi

echo "[busybox] 交叉编译 ($CROSS) ..."
make -C "$SRC" -j"$(nproc)" CROSS_COMPILE="$CROSS"
make -C "$SRC" install CROSS_COMPILE="$CROSS"

if [ ! -x "$SRC/_install/bin/busybox" ]; then
  echo "[busybox] FATAL: _install/bin/busybox 不存在" >&2
  exit 1
fi
echo "[busybox] OK -> $SRC/_install"
