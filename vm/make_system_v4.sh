#!/bin/bash
# make_system_v4.sh - 基于已验证的 v3 系统盘做「加法」注入，生成阶段 3 系统盘
#
# 只替换 / 新增文件，不推翻既有基线：
#   - /sbin/aether-compositor      ARM64 阶段 3 完整版（14 应用 + 18 项设置 + 液态玻璃）
#   - /etc/aether/phonetest.sh     阶段 3 自检（14 应用 / 设置二级页 / 深浅两套）
#   - /etc/aether/version          系统版本（设置 → 系统更新 读取）
#   - /usr/share/aether/**         真实资产：4 张 PPM 壁纸 / 24 帧片源 / 3 首 WAV
# 输出 aetheros-system-v4.qcow2（与既有 v3 同规格 256MiB ext4）。
set -e

WORK=/workspace/aether-vm/build/work
BASE="$WORK/aetheros-system-v3-glass.qcow2"
OUT="$WORK/aetheros-system-v4.qcow2"
RAW="$WORK/v4.raw"
COMP=/root/aether-archive/aether-aarch64-build/root-full/sbin/aether-compositor
PHONETEST="$WORK/phonetest-v4.sh"
STAGE=/root/aether-archive/aether-aarch64-build/stage
ASSETS="$STAGE/usr/share/aether"
VERSION="$STAGE/etc/aether/version"
CMDF="$WORK/debugfs-v4.cmd"

[ -f "$BASE" ]   || { echo "缺少基线盘 $BASE"; exit 1; }
[ -f "$COMP" ]   || { echo "缺少 ARM64 合成器 $COMP"; exit 1; }
[ -f "$PHONETEST" ] || { echo "缺少自检脚本 $PHONETEST"; exit 1; }
[ -d "$ASSETS" ] || { echo "缺少资产目录 $ASSETS"; exit 1; }

# 自检脚本同步到 stage，保证 stage 与系统盘内容一致
cp "$PHONETEST" "$STAGE/etc/aether/phonetest.sh"
chmod 755 "$STAGE/etc/aether/phonetest.sh"

echo "[1/4] qcow2 -> raw"
qemu-img convert -O raw "$BASE" "$RAW"

echo "[2/4] 生成 debugfs 命令（先 rm 再 write，避免目标已存在导致覆盖失败）"
{
    echo "rm /sbin/aether-compositor"
    echo "write $COMP /sbin/aether-compositor"
    echo "sif /sbin/aether-compositor mode 0100755"
    echo "rm /etc/aether/phonetest.sh"
    echo "write $PHONETEST /etc/aether/phonetest.sh"
    echo "sif /etc/aether/phonetest.sh mode 0100755"
    echo "rm /etc/aether/version"
    echo "write $VERSION /etc/aether/version"
    # 目录逐层创建（已存在的会提示，但不影响后续命令）
    for d in /usr/share /usr/share/aether /usr/share/aether/wallpapers \
             /usr/share/aether/clips /usr/share/aether/clips/loop \
             /usr/share/aether/audio; do
        echo "mkdir $d"
    done
    for f in $(find "$ASSETS" -type f | sort); do
        rel=${f#"$ASSETS"/}
        echo "rm /usr/share/aether/$rel"
        echo "write $f /usr/share/aether/$rel"
    done
} > "$CMDF"

echo "[3/4] 注入 $(grep -c '^write ' "$CMDF") 个文件"
debugfs -w -f "$CMDF" "$RAW" > "$WORK/debugfs-v4.log" 2>&1 || true
# 只把「目标不存在 / 目录已存在」视为可忽略，真正的文件写入结果下面逐项校验
grep -E "error|Error|cannot|Cannot|failed|Failed" "$WORK/debugfs-v4.log" | head -20 || true

echo "[4/4] 校验注入结果并回写 qcow2"
fail=0
check_file() {
    local path="$1" expect="$2"
    local got
    got=$(debugfs -R "stat $path" "$RAW" 2>/dev/null | grep -oE 'Size: [0-9]+' | head -1 | awk '{print $2}')
    if [ "$got" != "$expect" ]; then
        echo "  !! $path 期望 $expect 字节，实际 '${got:-缺失}'"
        fail=1
    else
        echo "  ok $path ($got 字节)"
    fi
}
check_file /sbin/aether-compositor "$(stat -c %s "$COMP")"
check_file /etc/aether/phonetest.sh "$(stat -c %s "$PHONETEST")"
check_file /usr/share/aether/wallpapers/wall-01.ppm "$(stat -c %s "$ASSETS/wallpapers/wall-01.ppm")"
check_file /usr/share/aether/clips/loop/f000.ppm "$(stat -c %s "$ASSETS/clips/loop/f000.ppm")"
check_file /usr/share/aether/audio/overture.wav "$(stat -c %s "$ASSETS/audio/overture.wav")"
[ "$fail" = 0 ] || { echo "注入校验失败，保留 raw 供排查：$RAW"; exit 1; }

qemu-img convert -O qcow2 "$RAW" "$OUT"
rm -f "$RAW"
echo "完成: $OUT"
qemu-img info "$OUT" | sed -n '1,6p'
