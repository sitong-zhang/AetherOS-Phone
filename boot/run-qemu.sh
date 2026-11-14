#!/bin/bash
# run-qemu.sh - A 轨整机冷启动、图形首帧与串口取证
#
# QEMU aarch64 (-M virt, TCG) → Linux 6.6.70 Image → 自建 ext4 根盘 →
# 自研 PID 1 aether-init → 服务编排（含合成器）→ virtio-gpu 图形上屏 →
# 开机诊断完成标记 + QMP screendump 抓取真实首帧。
# 串口日志与图形证据写入仓库外本地证据目录。
set -euo pipefail

BOOT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BOOT/.." && pwd)"
BUILD="${AETHER_BUILD:-$REPO/build/aarch64}"
EVID="${AETHER_EVIDENCE:-$REPO/evidence/round-009}"
LOG="$EVID/boot-serial.log"
QMP="$EVID/aether-qmp.sock"
FRAME_PNG="$EVID/first-frame.png"
FRAME_JSON="$EVID/first-frame.json"
IMAGE="$BUILD/Image"
ROOTFS="$BUILD/rootfs.ext4"
TIMEOUT="${BOOT_TIMEOUT:-90}"

command -v qemu-system-aarch64 >/dev/null 2>&1 || {
    echo "[run][ERROR] 缺依赖: qemu-system-aarch64" >&2; exit 1; }
[ -f "$IMAGE" ]  || { echo "[run][ERROR] 缺内核镜像: $IMAGE（先跑 boot/build.sh）" >&2; exit 1; }
[ -f "$ROOTFS" ] || { echo "[run][ERROR] 缺根文件系统: $ROOTFS（先跑 boot/build.sh）" >&2; exit 1; }

mkdir -p "$EVID"
: > "$LOG"
rm -f "$QMP"

echo "[run] QEMU aarch64 TCG 冷启动（virtio-gpu）-> $LOG"
timeout "$TIMEOUT" qemu-system-aarch64 \
    -M virt -cpu cortex-a57 -m 512M -smp 2 \
    -kernel "$IMAGE" \
    -drive "file=$ROOTFS,format=raw,if=virtio" \
    -append "root=/dev/vda rw console=ttyAMA0 init=/init panic=5 loglevel=7" \
    -device virtio-gpu-pci \
    -display none -serial "file:$LOG" -monitor none -no-reboot \
    -qmp "unix:$QMP,server=on,wait=off" \
    > "$EVID/qemu.stdout.txt" 2> "$EVID/qemu.stderr.txt" &
QPID=$!

# 等待开机链路完成（串口诊断标记）
ok=0
for i in $(seq 1 "$TIMEOUT"); do
    if grep -q 'AETHEROS_BOOT_DIAG_DONE=1' "$LOG" 2>/dev/null; then
        ok=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        break
    fi
    sleep 1
done
if [ "$ok" -ne 1 ]; then
    echo "[run][ERROR] 未检测到开机诊断完成标记" >&2
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    exit 1
fi
echo "[run] 开机链路完成：检测到诊断完成标记（第 ${i}s）"

# 等待合成器完成首帧上屏（其日志走串口 console），再多给一点渲染时间
for j in $(seq 1 12); do
    if grep -qiE 'compositor.*(显示打开|serving on|逻辑画布)' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done
sleep 3

echo "[run] 抓取 virtio-gpu 真实图形首帧（QMP screendump）"
CAP_RC=0
python3 "$REPO/tools/capture_frame.py" \
    --qmp "$QMP" --out "$FRAME_PNG" --json "$FRAME_JSON" \
    --frames 2 --interval 1.0 || CAP_RC=$?
echo "[run] 抓帧 rc=$CAP_RC（4=空白帧，3=QMP 失败）"

kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
exit "$CAP_RC"
