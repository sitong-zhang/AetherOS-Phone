#!/bin/bash
# build.sh - A 轨整机链路构建
#
# 产出（全部落在 build/aarch64/，由 .gitignore 排除）：
#   Image        Linux 6.6.70 ARM64 内核镜像（从源码交叉编译）
#   root/        同一份 guest/src 源码交叉编译出的 aarch64 用户态
#   secure/      同一份 secure/ 源码交叉编译出的 aarch64 安全引擎
#   rootfs.ext4  自建 ext4 根盘：BusyBox 基座 + 用户态 + 安全引擎 +
#                init 服务清单 + 开机诊断 + 安全自检
#
# 用户态与安全引擎源码和调试环境原生构建、真机构建同源，仅构建目标不同。
set -euo pipefail

BOOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BOOT/.." && pwd)"
BUILD="${AETHER_BUILD:-$REPO/build/aarch64}"
KVER="6.6.70"
KSRC="$BUILD/linux-$KVER"
IMAGE="$BUILD/Image"
USERLAND="$BUILD/root"
SECURE="$BUILD/secure"
STAGE="$BUILD/stage"
ROOTFS="$BUILD/rootfs.ext4"
JOBS="${JOBS:-$(nproc)}"
SIZE="${ROOTFS_SIZE:-256}"   # MB

need() {
    command -v "$1" >/dev/null 2>&1 || { echo "[build][ERROR] 缺依赖: $1" >&2; exit 1; }
}
for c in aarch64-linux-gnu-gcc make curl sha256sum tar mkfs.ext4 truncate file; do need "$c"; done

mkdir -p "$BUILD"

echo "[build] === 1/5 外部资产获取与校验 ==="
bash "$BOOT/fetch-assets.sh" "$BUILD"

echo "[build] === 2/5 Linux $KVER ARM64 内核（从源码交叉编译） ==="
if [ -f "$IMAGE" ]; then
    echo "[build] 复用已有内核镜像（如需重编请删除 $IMAGE）"
else
    cp "$BOOT/kernel-$KVER.config" "$KSRC/.config"
    make -C "$KSRC" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig >/dev/null
    make -C "$KSRC" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$JOBS" Image
    cp "$KSRC/arch/arm64/boot/Image" "$IMAGE"
fi
file "$IMAGE" | sed 's/,.*//'

echo "[build] === 3/5 aarch64 用户态（guest/src 同源交叉编译） ==="
AETHER_BUILD="$USERLAND" bash "$REPO/guest/build-aarch64.sh"

echo "[build] === 4/5 aarch64 安全引擎（secure/ 同源交叉编译） ==="
AETHER_SECURE_OUT="$SECURE" bash "$REPO/secure/build-aarch64.sh"

echo "[build] === 5/5 ext4 根文件系统 ==="
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,sbin,etc/aether/init.d,proc,sys,dev,dev/pts,tmp,run,mnt,data,var,usr/bin,usr/sbin}

# BusyBox 静态基座 + applet 链接（真机阶段替换为对应架构基座）
cp "$BOOT/assets/busybox-aarch64" "$STAGE/bin/busybox"
chmod 755 "$STAGE/bin/busybox"
for a in sh ash ls cat echo mkdir rm cp mv ln chmod mount umount ps kill sleep date \
         uname hostname awk sed grep head tail tr wc printf basename dirname find \
         xargs env sync dmesg free df du test true false stat touch readlink seq \
         sort uniq cut ip ifconfig ping hexdump dd; do
    ln -sf busybox "$STAGE/bin/$a"
done

# 同一份源码编译出的自研用户态与安全引擎
cp -a "$USERLAND/sbin/." "$STAGE/sbin/"
cp -a "$USERLAND/bin/." "$STAGE/bin/"
cp "$SECURE/sbin/aether-os" "$STAGE/sbin/aether-os"
chmod 755 "$STAGE/sbin/aether-os"

# init 服务清单、开机诊断与安全自检脚本
cp -a "$BOOT/rootfs/init.d/." "$STAGE/etc/aether/init.d/"
cp -a "$BOOT/rootfs/etc/." "$STAGE/etc/aether/"
chmod +x "$STAGE"/etc/aether/*.sh 2>/dev/null || true
ln -sf sbin/aether-init "$STAGE/init"

printf 'root:x:0:0:root:/root:/bin/sh\n' > "$STAGE/etc/passwd"
printf 'root:x:0:\n'                    > "$STAGE/etc/group"
printf 'aetheros\n'                    > "$STAGE/etc/hostname"

truncate -s "${SIZE}M" "$ROOTFS"
mkfs.ext4 -q -F -d "$STAGE" -b 4096 "$ROOTFS"

echo "[build] 产物:"
ls -lh "$IMAGE" "$ROOTFS"
echo "[build] 完成"
