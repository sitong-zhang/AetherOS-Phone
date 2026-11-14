#!/bin/bash
# fetch-assets.sh - A 轨外部资产获取与 SHA-256 校验
#
# 资产：
#   - Linux 6.6.70 源码 tarball：不随仓库分发，缺失时按镜像优先级下载并校验
#   - aarch64 BusyBox 静态基座：随仓库分发，校验其完整性
# 所有路径由脚本位置或 AETHER_BUILD 推导，不依赖机器绝对路径。
set -euo pipefail

BOOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BOOT/.." && pwd)"
BUILD="${1:-${AETHER_BUILD:-$REPO/build/aarch64}}"
KVER="6.6.70"
TARBALL="$BUILD/linux-$KVER.tar.xz"
KSRC="$BUILD/linux-$KVER"
CHECKSUM="$BOOT/ASSETS.sha256"
BB="$BOOT/assets/busybox-aarch64"

command -v curl     >/dev/null 2>&1 || { echo "[fetch][ERROR] 缺依赖: curl" >&2; exit 1; }
command -v sha256sum >/dev/null 2>&1 || { echo "[fetch][ERROR] 缺依赖: sha256sum" >&2; exit 1; }

mkdir -p "$BUILD"

# 从清单取期望校验和
expect_hash() {
    awk -v n="$1" '$2 == n || $2 == "*" n { print $1; exit }' "$CHECKSUM"
}
file_hash() { sha256sum "$1" | awk '{print $1}'; }
tarball_ok() { [ -f "$TARBALL" ] && [ "$(file_hash "$TARBALL")" = "$(expect_hash "linux-$KVER.tar.xz")" ]; }

# 1) BusyBox 基座（随仓库分发，先校验）
BB_EXPECT="$(expect_hash 'boot/assets/busybox-aarch64')"
if [ -z "$BB_EXPECT" ] || [ ! -f "$BB" ] || [ "$(file_hash "$BB")" != "$BB_EXPECT" ]; then
    echo "[fetch][ERROR] BusyBox 基座缺失或 SHA-256 校验失败: $BB" >&2
    exit 1
fi

# 2) 内核源码：源码树已存在则跳过；否则校验缓存 tarball，失效则按镜像优先级重新下载
if [ -d "$KSRC" ]; then
    echo "[fetch] 内核源码树已就绪: $KSRC"
else
    if ! tarball_ok; then
        rm -f "$TARBALL"
        for url in \
            "https://mirrors.tuna.tsinghua.edu.cn/kernel/v6.x/linux-$KVER.tar.xz" \
            "https://mirrors.aliyun.com/linux-kernel/v6.x/linux-$KVER.tar.xz" \
            "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz"; do
            echo "[fetch] 下载内核源码: $url"
            if curl -fL --retry 2 --max-time 600 -o "$TARBALL" "$url" && tarball_ok; then
                break
            fi
            rm -f "$TARBALL"
        done
    fi
    if [ ! -f "$TARBALL" ]; then
        echo "[fetch][ERROR] 内核源码下载失败（三个镜像均不可用）" >&2
        exit 1
    fi
    if ! tarball_ok; then
        echo "[fetch][ERROR] 内核源码 SHA-256 与 ASSETS.sha256 不一致，拒绝使用" >&2
        exit 1
    fi
    echo "[fetch] 解压内核源码"
    tar -xf "$TARBALL" -C "$BUILD"
fi

echo "[fetch] 资产齐备: $KSRC / $BB"
