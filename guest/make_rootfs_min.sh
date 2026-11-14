#!/bin/bash
# make_rootfs_aarch64.sh - 生成 AetherOS 手机版 aarch64 根文件系统镜像 (ext4)
# 与 x86_64 流程同构，但：
#   - busybox 基座为交叉编译的静态 aarch64 版
#   - 用户态为交叉编译的 aarch64 版（fbdev-only，无 libdrm）
#   - 显式建立 /init -> /sbin/aether-init 作为 PID 1 入口（适配 -M virt 默认 init 查找）
#   - 串口诊断用 /dev/console（aarch64 PL011 映射到 ttyAMA0）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
BUILD="${AETHER_BUILD:-$REPO/build/aarch64}"
STAGE="$BUILD/stage"
BB="${BB_DIR:-$REPO/build/host/busybox/_install}"
BIN="${BIN_DIR:-$BUILD/root}"
IMG="${ROOTFS:-$BUILD/aetheros-phone-aarch64.ext4}"
SIZE="${SIZE:-256}"   # MB

echo "[rootfs-aarch64] 清理并重建 staging: $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "[rootfs-aarch64] 复制静态 busybox 基座 (aarch64)"
cp -a "$BB/." "$STAGE/"

echo "[rootfs-aarch64] 复制 AetherOS 用户态 (aarch64)"
cp -a "$BIN/." "$STAGE/"

echo "[rootfs-aarch64] 创建目录结构"
mkdir -p "$STAGE"/{proc,sys,dev,dev/pts,tmp,run,run/aether,mnt/share,data/aether/crash,etc/aether/init.d,etc/aether,var/log}
chmod 1777 "$STAGE/tmp"

echo "[rootfs-aarch64] PID 1 入口: /init -> /sbin/aether-init"
ln -sf /sbin/aether-init "$STAGE/init"
ln -sf /sbin/aether-init "$STAGE/sbin/init"   # 覆盖 busybox init（busybox 仍保留为救援壳）

echo "[rootfs-aarch64] 写入基础配置文件"
cat > "$STAGE/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > "$STAGE/etc/group" <<'EOF'
root:x:0:
EOF
echo "aetheros" > "$STAGE/etc/hostname"
cat > "$STAGE/etc/profile" <<'EOF'
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
hostname aetheros
EOF

echo "[rootfs-aarch64] 写入服务配置 (/etc/aether/init.d)"
cat > "$STAGE/etc/aether/init.d/aetherosd" <<'EOF'
name=aetherosd
exec=/sbin/aetherosd
restart=always
EOF

PROFILE="${AETHER_PROFILE:-phone}"
if [ "$PROFILE" = "phone" ]; then
  RES_LINE='export AETHER_RES=720x1280'
  PHONE_LINE='export AETHER_PHONE=1'
  BACKEND_LINE='export AETHER_BACKEND=fbdev'
else
  RES_LINE='# 桌面形态：不强制竖屏'
  PHONE_LINE='# 桌面形态：不进入手机外壳'
  BACKEND_LINE='# 桌面形态：后端自动'
fi
cat > "$STAGE/etc/aether/run-compositor" <<EOF
#!/bin/sh
# 手机版默认竖屏分辨率；aarch64 编译为 fbdev-only（真实 Limbo 走 /dev/fb0），
# 无 /dev/fb0 时合成器自动进入 headless 渲染模式（仍渲染画布+IPC+screenshot）。
# 合成器把手机 UI 渲染到 720x1280 逻辑画布，再缩放进任意物理帧缓冲（DRM/fbdev 均可）。
$RES_LINE
$PHONE_LINE
$BACKEND_LINE
exec /sbin/aether-compositor
EOF
chmod +x "$STAGE/etc/aether/run-compositor"

cat > "$STAGE/etc/aether/init.d/aether-compositor" <<'EOF'
name=aether-compositor
exec=/etc/aether/run-compositor
depends=aetherosd
restart=always
EOF

cat > "$STAGE/etc/aether/init.d/aether-watchdog" <<'EOF'
name=aether-watchdog
exec=/sbin/aether-watchdog
depends=aether-compositor
restart=always
EOF

cat > "$STAGE/etc/aether/init.d/bootdiag" <<'EOF'
name=bootdiag
exec=/etc/aether/bootdiag.sh
depends=aether-compositor
restart=never
EOF

cat > "$STAGE/etc/aether/init.d/pcshot" <<'EOF'
name=pcshot
exec=/etc/aether/pcshot.sh
depends=aether-compositor
restart=never
EOF

echo "[rootfs-aarch64] 写入手机自检脚本（仅 aether.phonetest=1 时启用，9p 共享导出逐帧证据）"
cat > "$STAGE/etc/aether/phonetest.sh" <<'EOF'
#!/bin/sh
# AetherOS 最小闭环自检：开机 -> 锁屏 -> 桌面(仅设置) -> 点击设置 -> 设置窗口
# 由内核参数 aether.phonetest=1 触发；逐帧证据经 9p 导出到宿主共享目录。
grep -q "aether.phonetest=1" /proc/cmdline 2>/dev/null || exit 0
I=/bin/aether-ipc
T=/dev/console
mkdir -p /data/aether /mnt/share
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/share 2>/dev/null
OUT=/data/aether
grep -q " /mnt/share " /proc/mounts 2>/dev/null && OUT=/mnt/share
i=0
while [ $i -lt 40 ] && [ ! -S /run/aetheros-comp.sock ]; do i=$((i+1)); sleep 1; done
[ -S /run/aetheros-comp.sock ] || { echo "SELFTEST: no compositor socket" > $T; exit 0; }
shot(){ aether-shot "$OUT/$1" 2>/dev/null && echo "SELFTEST: shot $1 -> $OUT" > $T; }

# 1) 锁屏帧
shot 01-lock.ppm
sleep 2

# 2) 解锁 -> 桌面（桌面上只有「设置」一个图标）
$I unlock >/dev/null 2>&1
$I home   >/dev/null 2>&1
sleep 1
shot 02-home-settings-only.ppm
echo "SELFTEST_STATE_HOME=$( $I state 2>/dev/null )" > $T

# 3) 真实点击居中的「设置」图标（720x1280 下图标中心约 360,618）
$I click 360 618 >/dev/null 2>&1
sleep 1
shot 03-settings-clicked.ppm
echo "SELFTEST_STATE_CLICK=$( $I state 2>/dev/null )" > $T

# 4) 兜底：若点击后仍无窗口，直接用 open 5 (APP_SETTINGS) 打开，确保「能打开」被验证
$I state 2>/dev/null | grep -q '"open":0' && { \
    $I open 5 >/dev/null 2>&1; sleep 1; shot 04-settings-open.ppm; \
    echo "SELFTEST_FALLBACK_OPEN=1" > $T; }

echo "SELFTEST_STATE_FINAL=$( $I state 2>/dev/null )" > $T
sync
echo "SELFTEST_DONE=1" > $T
EOF
chmod +x "$STAGE/etc/aether/phonetest.sh"

cat > "$STAGE/etc/aether/init.d/phonetest" <<'EOF'
name=phonetest
exec=/etc/aether/phonetest.sh
depends=aether-compositor
restart=never
EOF

echo "[rootfs-aarch64] 写入诊断 / 截图脚本"
cat > "$STAGE/etc/aether/bootdiag.sh" <<'EOF'
#!/bin/sh
i=0
while [ $i -lt 40 ]; do
  if [ -S /run/aetheros-comp.sock ] && [ -S /run/aetheros.sock ]; then
    break
  fi
  i=$((i+1)); sleep 1
done
SYS=$(aether-sysinfo system 2>/dev/null)
echo "AETHEROS_BOOT_DIAG: system=$SYS"
echo "AETHEROS_BOOT_DIAG_FB: fb0=$(test -e /dev/fb0 && echo yes || echo no) dri=$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
echo "AETHEROS_BOOT_DIAG_FB: procfb=$(cat /proc/fb 2>/dev/null | tr '\n' ' ')"
echo "AETHEROS_BOOT_DIAG_FB: res=${AETHER_RES:-unset} backend=${AETHER_BACKEND:-auto}"
echo "AETHEROS_BOOT_DIAG_SERVICES:"
aether-sysinfo services 2>/dev/null | sed 's/^/  /'
echo "AETHEROS_BOOT_DIAG_DONE=1"
echo "1" > /run/aether/diag-done 2>/dev/null
EOF
chmod +x "$STAGE/etc/aether/bootdiag.sh"

cat > "$STAGE/etc/aether/pcshot.sh" <<'EOF'
#!/bin/sh
mkdir -p /mnt/share /run/aether
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/share 2>/dev/null
i=0
while [ $i -lt 30 ] && [ ! -S /run/aetheros-comp.sock ]; do
  i=$((i+1)); sleep 1
done
[ -S /run/aetheros-comp.sock ] || exit 0
sleep 2
if grep -q " /mnt/share " /proc/mounts 2>/dev/null; then
  aether-shot /mnt/share/pc-shot.ppm 2>/dev/null && echo "pcshot: 截图 -> 9p 共享"
  cp /run/aether/services.tsv /mnt/share/services.txt 2>/dev/null
else
  aether-shot /run/aether/boot-shot.ppm 2>/dev/null && echo "pcshot: 截图 -> /run/aether/boot-shot.ppm (无 9p)"
fi
EOF
chmod +x "$STAGE/etc/aether/pcshot.sh"

echo "[rootfs-aarch64] 生成 ext4 镜像 (mke2fs -d, ${SIZE}MB): $IMG"
rm -f "$IMG"
mkdir -p "$(dirname "$IMG")"
mke2fs -q -d "$STAGE" -t ext4 -b 4096 -L aetheros -O ^metadata_csum "$IMG" "${SIZE}M"
echo "[rootfs-aarch64] 完成: $IMG"
ls -la "$IMG"
