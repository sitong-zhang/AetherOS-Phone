#!/bin/bash
# 打包 AetherOS phone 系统镜像 tarball。
# 关键：把版本文件 etc/aether-version 一并打进包顶层，APK 解包后
# 落在 <workDir>/system/etc/aether-version，AetherCore.readImageVersion()
# 即可读出真实 release tag，避免仅靠下载 URL 推断（镜像站重定向下会丢）。
set -euo pipefail
cd "$(dirname "$0")"

VERSION="${1:-phone-v8}"

# 写入版本标记（与 make_rootfs.sh 写入 rootfs /etc/aether-version 同源）
mkdir -p etc
printf '%s\n' "$VERSION" > etc/aether-version

# 内核 / rootfs 必须存在
for f in aetheros-kernel-aarch64.Image aetheros-rootfs.ext4; do
  [ -f "$f" ] || { echo "FATAL: 缺少 $f" >&2; exit 1; }
done

tar czf aetheros-phone.tar.gz \
  aetheros-kernel-aarch64.Image \
  aetheros-rootfs.ext4 \
  phone-boot.sh \
  aether-android-bridge.sh \
  README-phone.md \
  etc/aether-version

echo "built aetheros-phone.tar.gz (version: $VERSION, $(du -h aetheros-phone.tar.gz | cut -f1))"
