#!/bin/bash
# make_system_v5.sh - 基于已验证的 v4 系统盘做「加法」注入，生成阶段 4 系统盘
#
# 只替换 / 新增文件，不推翻既有基线：
#   - /sbin/aether-compositor       ARM64 阶段 4 完整版（灵动岛/隐私/全局搜索/负一屏）
#   - /etc/aether/phonetest.sh      阶段 4 自检（五场景 + 三态 + 隐私 + 搜索 + 负一屏）
#   - /etc/aether/version           系统版本
#   - /data/aether/*.conf           本机真实数据：离线路线 / 联系人 / 书签
# v4 已有的 /usr/share/aether/** 壁纸、片源、音频原样保留。
# 输出 aetheros-system-v5.qcow2。
set -e

WORK=/workspace/aether-vm/build/work
BASE="$WORK/aetheros-system-v4.qcow2"
OUT="$WORK/aetheros-system-v5.qcow2"
RAW="$WORK/v5.raw"
COMP=/root/aether-archive/aether-aarch64-build/root-full/sbin/aether-compositor
PHONETEST="$WORK/phonetest-v5.sh"
STAGE=/root/aether-archive/aether-aarch64-build/stage
VERSION="$STAGE/etc/aether/version"
DATA="$STAGE/data/aether"
CMDF="$WORK/debugfs-v5.cmd"

[ -f "$BASE" ]   || { echo "缺少基线盘 $BASE"; exit 1; }
[ -f "$COMP" ]   || { echo "缺少 ARM64 合成器 $COMP"; exit 1; }
[ -f "$PHONETEST" ] || { echo "缺少自检脚本 $PHONETEST"; exit 1; }
[ -f "$VERSION" ] || { echo "缺少版本文件 $VERSION"; exit 1; }
[ -d "$DATA" ]   || { echo "缺少数据目录 $DATA"; exit 1; }

# 自检脚本同步到 stage，保证 stage 与系统盘内容一致
cp "$PHONETEST" "$STAGE/etc/aether/phonetest.sh"
chmod 755 "$STAGE/etc/aether/phonetest.sh"

echo "[1/4] qcow2 -> raw"
qemu-img convert -O raw "$BASE" "$RAW"

echo "[2/4] 生成 debugfs 命令"
{
    echo "rm /sbin/aether-compositor"
    echo "write $COMP /sbin/aether-compositor"
    echo "sif /sbin/aether-compositor mode 0100755"
    echo "rm /etc/aether/phonetest.sh"
    echo "write $PHONETEST /etc/aether/phonetest.sh"
    echo "sif /etc/aether/phonetest.sh mode 0100755"
    echo "rm /etc/aether/version"
    echo "write $VERSION /etc/aether/version"
    echo "mkdir /data"
    echo "mkdir /data/aether"
    for f in route.conf contacts.conf bookmarks.conf; do
        echo "rm /data/aether/$f"
        echo "write $DATA/$f /data/aether/$f"
    done
} > "$CMDF"

echo "[3/4] 注入 $(grep -c '^write ' "$CMDF") 个文件"
debugfs -w -f "$CMDF" "$RAW" > "$WORK/debugfs-v5.log" 2>&1 || true
grep -E "error|Error|cannot|Cannot|failed|Failed" "$WORK/debugfs-v5.log" | head -20 || true

echo "[4/4] 校验注入结果并回写 qcow2"
fail=0
check_file() {
    local path="$1" expect="$2" got
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
check_file /data/aether/route.conf "$(stat -c %s "$DATA/route.conf")"
check_file /data/aether/contacts.conf "$(stat -c %s "$DATA/contacts.conf")"
check_file /data/aether/bookmarks.conf "$(stat -c %s "$DATA/bookmarks.conf")"
check_file /usr/share/aether/wallpapers/wall-01.ppm "$(stat -c %s "$STAGE/usr/share/aether/wallpapers/wall-01.ppm")"
[ "$fail" = 0 ] || { echo "注入校验失败，保留 raw 供排查：$RAW"; exit 1; }

qemu-img convert -O qcow2 "$RAW" "$OUT"
rm -f "$RAW"
echo "完成: $OUT"
qemu-img info "$OUT" | sed -n '1,6p'
