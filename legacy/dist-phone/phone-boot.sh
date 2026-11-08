#!/bin/sh
# AetherOS 手机启动器 — Termux + QEMU 纯软件模拟 (无需 root, 无需 KVM)
#
# 用法:
#   ./phone-boot.sh              # 默认 512M 内存、竖屏 720x1600 启动
#   AETHER_MEM=384M ./phone-boot.sh   # 低内存手机 (3GB 以下)
#   AETHER_SMP=1 ./phone-boot.sh      # 单 vCPU, TCG 下更稳
#   AETHER_RES=1080x1920 ./phone-boot.sh  # 换分辨率（宽x高）
#   # 极慢手机（冷启动超过 2 分钟）：放宽看门狗启动宽限期到 300s
#   AETHER_APPEND="console=ttyAMA0 root=/dev/vda rw ip=dhcp aether_wdt_grace=300" ./phone-boot.sh
#
# 画面: VNC 查看器 (推荐 AVNC) 连接 127.0.0.1:5900
#       默认竖屏 720x1600（1080x2400 手机的原生 9:20 比例）——VNC 里
#       铺满整块手机屏，AetherOS 就是一部手机，不是桌面窗口。
# 输入: VNC 窗口里打字 = 客机键盘; 开机先【手指点按屏幕任意处解锁】,
#       再【点按 Launcher 应用行】启动应用（virtio-tablet 提供绝对指针/触摸，
#       compositor 把点击路由给 SystemUI：锁屏态点按=解锁，启动器态行命中=启动）；
#       数字键 1-9 亦可启动应用（仅解锁后有效，键盘兜底）
# 内核日志: 本终端直接滚动（AETHER_LOGS=1 时前台运行；默认由 boot-latest 后台跑）
# 退出: Ctrl-A 然后按 X
set -u
cd "$(dirname "$0")"

KERNEL=aetheros-kernel-aarch64.Image
ROOTFS=aetheros-rootfs.ext4
MEM="${AETHER_MEM:-512M}"
SMP="${AETHER_SMP:-2}"
RES="${AETHER_RES:-720x1600}"
GPU_XRES="${RES%x*}"
GPU_YRES="${RES#*x}"
APPEND="${AETHER_APPEND:-console=ttyAMA0 root=/dev/vda rw ip=dhcp}"

if [ ! -f "$KERNEL" ] || [ ! -f "$ROOTFS" ]; then
    echo "[aether] 缺少 $KERNEL 或 $ROOTFS, 请先解压完整镜像包"; exit 1
fi
if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[aether] 未安装 qemu, 请先执行: pkg install qemu-system-aarch64-headless"; exit 1
fi

echo "[aether] 启动 AetherOS (TCG 软件模拟, 首次启动约需 1-3 分钟)..."
echo "[aether] 画面: 用 AVNC 连接 127.0.0.1:5900 (竖屏 ${GPU_XRES}x${GPU_YRES}) | 退出: Ctrl-A X"
echo "[aether] 开机: 先点按屏幕解锁 -> 再点按启动器应用行打开应用"

# Android 宿主 HAL 桥接：把真实电量/网络经 9p 共享目录喂给客户机。
# 有 termux-api 则用真实数据，没有则退化为 sysfs 电量；都没有时客户机
# 显示 bat --（诚实标记，不伪造）。桥接可选，缺失不影响系统启动。
SHARE="${AETHER_SHARE:-$(pwd)/share}"
mkdir -p "$SHARE"
BRIDGE_PID=""
if [ -f ./aether-android-bridge.sh ]; then
    sh ./aether-android-bridge.sh "$SHARE" >/dev/null 2>&1 &
    BRIDGE_PID=$!
    echo "[aether] Android 宿主桥接已启动 (电量/网络 -> 9p share)"
fi
cleanup() { [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

qemu-system-aarch64 \
  -M virt -cpu cortex-a57 -smp "$SMP" -m "$MEM" \
  -kernel "$KERNEL" \
  -drive if=virtio,format=raw,file="$ROOTFS" \
  -append "$APPEND" \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
  -virtfs local,path="$SHARE",mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -device virtio-gpu-pci,xres="$GPU_XRES",yres="$GPU_YRES" \
  -device virtio-keyboard-pci \
  -device virtio-tablet-pci \
  -vnc 127.0.0.1:0 \
  -snapshot \
  -serial stdio -monitor none
