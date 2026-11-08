#!/bin/bash
# boot-pc.sh - 在 QEMU 中启动 AetherOS PC（x86_64）
# 默认无头（串口日志落到 stdio）；--gui 打开图形窗口。
# 默认导出 9p 共享到 $SHARE，使客机 pcshot 能把截图落到宿主机。
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
KERNEL="${KERNEL:-$ROOT/aetheros-kernel-6.6.70-x86_64.bzImage}"
ROOTFS="${ROOTFS:-/tmp/aether-pc-build/aetheros-rootfs-pc.ext4}"
SHARE="${SHARE:-/tmp/aether-pc-share}"
MEM="${MEM:-2048}"
SMP="${SMP:-2}"
GUI=0
for a in "$@"; do
  [ "$a" = "--gui" ] && GUI=1
done

[ -f "$KERNEL" ] || { echo "FATAL: 内核不存在 $KERNEL" >&2; exit 2; }
[ -f "$ROOTFS" ]  || { echo "FATAL: rootfs 不存在 $ROOTFS（先 bash make_rootfs_pc.sh）" >&2; exit 2; }

mkdir -p "$SHARE"

echo "=== AetherOS PC 启动 ==="
echo "内核 : $KERNEL"
echo "根fs : $ROOTFS"
echo "共享 : $SHARE (9p mount_tag=hostshare -> /mnt/share)"

if [ "$GUI" = "1" ]; then
  DISP="-display gtk"
else
  DISP="-nographic -serial stdio -monitor none"
fi

qemu-system-x86_64 \
  -kernel "$KERNEL" \
  -append "root=/dev/vda console=ttyS0,115200 loglevel=4 rw init=/sbin/aether-init" \
  -drive file="$ROOTFS",format=raw,if=virtio \
  -device virtio-gpu-pci,xres=1280,yres=720 \
  -device virtio-keyboard-pci \
  -device virtio-tablet-pci \
  -device virtio-mouse-pci \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -m "$MEM" -smp "$SMP" -accel tcg \
  -device virtio-9p-pci,fsdev=sh0,mount_tag=hostshare \
  -fsdev local,id=sh0,path="$SHARE",security_model=none \
  $DISP
