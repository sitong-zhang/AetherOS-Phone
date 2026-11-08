#!/bin/sh
# 启动 AetherOS 真 Linux 内核 (aarch64)
# 真实磁盘 rootfs + virtio 网络(DHCP) + 9p 宿主共享目录
#
# 前置: 本机安装 qemu-system-arm (apt-get install qemu-system-arm)
# 用法:   ./boot.sh
# 退出:   Ctrl-A 然后按 X
KERNEL=aetheros-kernel-6.6.70-aarch64.Image
ROOTFS=aetheros-rootfs.ext4
SHARE=hostshare

mkdir -p "$SHARE"
qemu-system-aarch64 \
  -M virt -cpu cortex-a57 -smp 2 -m 512M \
  -kernel "$KERNEL" \
  -drive if=virtio,format=raw,file="$ROOTFS" \
  -append "console=ttyAMA0 root=/dev/vda rw ip=dhcp" \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-device,netdev=net0 \
  -virtfs local,path="$SHARE",mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -nographic
