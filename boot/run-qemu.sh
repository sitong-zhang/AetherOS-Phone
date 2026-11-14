#!/bin/bash
# run-qemu.sh - A 轨整机冷启动与串口取证
#
# QEMU aarch64 (-M virt, TCG) → Linux 6.6.70 Image → 自建 ext4 根盘 →
# 自研 PID 1 aether-init → 服务编排 → 开机诊断完成标记。
# 串口日志写入仓库外本地证据目录；检测到诊断完成标记即结束本次取证。
set -euo pipefail

BOOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BOOT/.." && pwd)"
BUILD="${AETHER_BUILD:-$REPO/build/aarch64}"
EVID="${AETHER_EVIDENCE:-$REPO/../aetheros-archive/evidence/round-007}"
LOG="$EVID/boot-serial.log"
IMAGE="$BUILD/Image"
ROOTFS="$BUILD/rootfs.ext4"
TIMEOUT="${BOOT_TIMEOUT:-90}"

command -v qemu-system-aarch64 >/dev/null 2>&1 || {
    echo "[run][ERROR] 缺依赖: qemu-system-aarch64" >&2; exit 1; }
[ -f "$IMAGE" ]  || { echo "[run][ERROR] 缺内核镜像: $IMAGE（先跑 boot/build.sh）" >&2; exit 1; }
[ -f "$ROOTFS" ] || { echo "[run][ERROR] 缺根文件系统: $ROOTFS" >&2; exit 1; }

mkdir -p "$EVID"
: > "$LOG"

echo "[run] QEMU aarch64 TCG 冷启动 -> $LOG"
timeout "$TIMEOUT" qemu-system-aarch64 \
    -M virt -cpu cortex-a57 -m 512M -smp 2 \
    -kernel "$IMAGE" \
    -drive "file=$ROOTFS,format=raw,if=virtio" \
    -append "root=/dev/vda rw console=ttyAMA0 init=/init panic=5 loglevel=7" \
    -display none -serial "file:$LOG" -monitor none -no-reboot \
    > "$EVID/qemu.stdout.txt" 2> "$EVID/qemu.stderr.txt" &
QPID=$!

for i in $(seq 1 "$TIMEOUT"); do
    if grep -q 'AETHEROS_BOOT_DIAG_DONE=1' "$LOG" 2>/dev/null; then
        echo "[run] 开机链路完成：检测到诊断完成标记（第 ${i}s）"
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        exit 0
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        echo "[run][ERROR] QEMU 提前退出，未检测到诊断完成标记" >&2
        wait "$QPID" 2>/dev/null || true
        exit 1
    fi
    sleep 1
done

echo "[run][ERROR] 超时 ${TIMEOUT}s，未检测到诊断完成标记" >&2
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
exit 1
