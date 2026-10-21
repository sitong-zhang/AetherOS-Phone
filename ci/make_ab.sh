#!/bin/bash
# ==============================================================================
#  make_ab.sh —— 构建 AetherOS A/B 双槽 OTA 测试镜像组（P1-2c）
#
#  产物（$OUT，默认 /tmp/aether-ab）：
#    slotA.ext4                  A 槽系统镜像（/etc/aether-slot = A）
#    slotB.ext4                  B 槽系统镜像（/etc/aether-slot = B）
#    persist.ext4                持久分区（初始槽位标记 slot=A）
#    aetheros-ab-initramfs.cpio.gz  启动选择器 initramfs（/init = ci/ab-initramfs-init.sh）
#    aether-ota-aarch64          aarch64 静态 OTA 管理器（槽内 + initramfs 共用）
#
#  设计：A/B 两槽是同一个系统镜像的两个副本（真实 OTA 语义：镜像相同、槽位
#  不同），靠 /etc/aether-slot 自证身份；initramfs 选择器按 /persist/slot
#  决定挂哪个盘。aether-ota --switch 翻转标记前先挂载目标槽校验 /sbin/init，
#  因此不存在"标记翻转后目标槽损坏导致变砖"的窗口。
#
#  可复现性：所有输入路径均可用环境变量覆盖，CI 与本地跑同一条构建路径。
# ==============================================================================
set -e
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BB=${BB:-/tmp/linux-build/busybox-1.36.1/_install}
ALIB=${ALIB:-/usr/aarch64-linux-gnu/lib}
OUT=${OUT:-/tmp/aether-ab}
CROSS=${CROSS:-aarch64-linux-gnu-}
AETHER_INIT=${AETHER_INIT:-$REPO_ROOT/aether-init-aarch64}
AETHEROSD=${AETHEROSD:-$REPO_ROOT/aetherosd-aarch64-static}
AETHER_COMP=${AETHER_COMP:-$REPO_ROOT/aether-compositor}
AETHER_DEMO=${AETHER_DEMO:-$REPO_ROOT/aether-demo}
# P2-11：包管理器三件套（宿主签名器 + 客机管理器 + 沙箱启动器）与签名密钥
AETHER_PKG_HOST=${AETHER_PKG_HOST:-$REPO_ROOT/build/aether-pkg-host}
AETHER_PKG=${AETHER_PKG:-$REPO_ROOT/build/aether-pkg-aarch64}
AETHER_SANDBOX_RUN=${AETHER_SANDBOX_RUN:-$REPO_ROOT/build/aether-sandbox-run}
AETHER_LOGD=${AETHER_LOGD:-$REPO_ROOT/build/aether-logd}
AETHER_LOGCTL=${AETHER_LOGCTL:-$REPO_ROOT/build/aether-logctl}
AETHER_CRASHD=${AETHER_CRASHD:-$REPO_ROOT/build/aether-crashd}
AETHER_WATCHDOG=${AETHER_WATCHDOG:-$REPO_ROOT/build/aether-watchdog}
AETHER_SYSUI=${AETHER_SYSUI:-$REPO_ROOT/build/aether-systemui}
AETHER_SYSUICTL=${AETHER_SYSUICTL:-$REPO_ROOT/build/aether-sysuictl}
AETHER_HAL_ANDROID=${AETHER_HAL_ANDROID:-$REPO_ROOT/build/aether-hal-android}
AETHER_SETTINGS=${AETHER_SETTINGS:-$REPO_ROOT/build/aether-settings}
AETHER_CLOCK=${AETHER_CLOCK:-$REPO_ROOT/build/aether-clock}
AETHER_SIGN_KEY=${AETHER_SIGN_KEY:-$REPO_ROOT/ci/test-signing.key}

if [ ! -x "$BB/bin/busybox" ]; then echo "FATAL: BusyBox 安装树不存在: $BB"; exit 1; fi
for f in "$AETHER_PKG_HOST" "$AETHER_PKG" "$AETHER_SANDBOX_RUN" \
         "$AETHER_LOGD" "$AETHER_LOGCTL" "$AETHER_CRASHD" "$AETHER_WATCHDOG" \
         "$AETHER_SYSUI" "$AETHER_SYSUICTL" "$AETHER_HAL_ANDROID" \
         "$AETHER_SETTINGS" "$AETHER_CLOCK"; do
  [ -x "$f" ] || { echo "FATAL: P2-11 二进制不存在: $f（先构建 aether-pkg/aether-sandbox-run）"; exit 1; }
done

mkdir -p "$OUT"
OTA_BIN="$OUT/aether-ota-aarch64"

# 1) 交叉编译 aether-ota（aarch64 静态；槽内 rootfs 与 initramfs 共用同一二进制）
if [ ! -x "$OTA_BIN" ]; then
  echo "[make-ab] cross-compiling aether-ota (aarch64 static)"
  "$CROSS"gcc -static -O2 -Wall -o "$OTA_BIN" "$REPO_ROOT/aether-ota.c"
fi
[ -x "$OTA_BIN" ] || { echo "FATAL: aether-ota 交叉编译失败"; exit 1; }

# 2) 两个槽位镜像（同一系统镜像，仅槽位标记不同）
echo "[make-ab] building slot A rootfs"
env BB="$BB" ALIB="$ALIB" \
    ROOT="$OUT/rootfs-A" IMG="$OUT/slotA.ext4" SLOT=A \
    AETHER_INIT="$AETHER_INIT" \
    AETHEROSD="$AETHEROSD" \
    AETHER_COMP="$AETHER_COMP" \
    AETHER_DEMO="$AETHER_DEMO" \
    AETHER_OTA="$OTA_BIN" \
    AETHER_PKG_HOST="$AETHER_PKG_HOST" \
    AETHER_PKG="$AETHER_PKG" \
    AETHER_SANDBOX_RUN="$AETHER_SANDBOX_RUN" \
    AETHER_LOGD="$AETHER_LOGD" \
    AETHER_LOGCTL="$AETHER_LOGCTL" \
    AETHER_CRASHD="$AETHER_CRASHD" \
    AETHER_WATCHDOG="$AETHER_WATCHDOG" \
    AETHER_SYSUI="$AETHER_SYSUI" \
    AETHER_SYSUICTL="$AETHER_SYSUICTL" \
    AETHER_HAL_ANDROID="$AETHER_HAL_ANDROID" \
    AETHER_SETTINGS="$AETHER_SETTINGS" \
    AETHER_CLOCK="$AETHER_CLOCK" \
    AETHER_SIGN_KEY="$AETHER_SIGN_KEY" \
    bash "$REPO_ROOT/make_rootfs.sh"

echo "[make-ab] building slot B rootfs"
env BB="$BB" ALIB="$ALIB" \
    ROOT="$OUT/rootfs-B" IMG="$OUT/slotB.ext4" SLOT=B \
    AETHER_INIT="$AETHER_INIT" \
    AETHEROSD="$AETHEROSD" \
    AETHER_COMP="$AETHER_COMP" \
    AETHER_DEMO="$AETHER_DEMO" \
    AETHER_OTA="$OTA_BIN" \
    AETHER_PKG_HOST="$AETHER_PKG_HOST" \
    AETHER_PKG="$AETHER_PKG" \
    AETHER_SANDBOX_RUN="$AETHER_SANDBOX_RUN" \
    AETHER_LOGD="$AETHER_LOGD" \
    AETHER_LOGCTL="$AETHER_LOGCTL" \
    AETHER_CRASHD="$AETHER_CRASHD" \
    AETHER_WATCHDOG="$AETHER_WATCHDOG" \
    AETHER_SYSUI="$AETHER_SYSUI" \
    AETHER_SYSUICTL="$AETHER_SYSUICTL" \
    AETHER_HAL_ANDROID="$AETHER_HAL_ANDROID" \
    AETHER_SETTINGS="$AETHER_SETTINGS" \
    AETHER_CLOCK="$AETHER_CLOCK" \
    AETHER_SIGN_KEY="$AETHER_SIGN_KEY" \
    bash "$REPO_ROOT/make_rootfs.sh"

# 3) 持久分区（ext4，初始标记 slot=A；恢复标志由 --mark-recovery 运行时写入）
PSTAGE="$OUT/persist-stage"
rm -rf "$PSTAGE" && mkdir -p "$PSTAGE"
printf 'A\n' > "$PSTAGE/slot"
rm -f "$OUT/persist.ext4"
mkfs.ext4 -F -q -L aetheros-persist -b 4096 -d "$PSTAGE" "$OUT/persist.ext4" 64M
rm -rf "$PSTAGE"

# 4) 启动选择器 initramfs：busybox 底座 + aarch64 动态库 + aether-ota + /init 选择器
IRD="$OUT/initramfs"
rm -rf "$IRD" && mkdir -p "$IRD"
cp -a "$BB"/. "$IRD"/
mkdir -p "$IRD"/{proc,sys,dev,dev/pts,tmp,run,var/log,root,mnt,newroot,persist,etc}
mkdir -p "$IRD"/lib
for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libresolv.so.2 \
           libgcc_s.so.1 libdl.so.2 libpthread.so.0; do
  cp -aL "$ALIB/$lib" "$IRD/lib/" 2>/dev/null || true
done
cp "$OTA_BIN" "$IRD/aether-ota"          # 恢复模式执行 --apply 用（静态，无需 libs）
chmod 755 "$IRD/aether-ota"
cp "$REPO_ROOT/ci/ab-initramfs-init.sh" "$IRD/init"
chmod 755 "$IRD/init"
rm -f "$OUT/aetheros-ab-initramfs.cpio.gz"
(cd "$IRD" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9) \
    > "$OUT/aetheros-ab-initramfs.cpio.gz"
chmod 644 "$OUT/aetheros-ab-initramfs.cpio.gz"
rm -rf "$IRD" "$OUT/rootfs-A" "$OUT/rootfs-B"

echo "AB_IMAGES_BUILD_OK"
ls -lh "$OUT"/slotA.ext4 "$OUT"/slotB.ext4 "$OUT"/persist.ext4 \
       "$OUT"/aetheros-ab-initramfs.cpio.gz
