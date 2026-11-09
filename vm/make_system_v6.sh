#!/bin/bash
# make_system_v6.sh - 基于已验证的 v5 系统盘做「加法」注入，生成阶段 6 系统盘
#
# 只替换 / 新增文件，不推翻既有基线：
#   - /sbin/aether-compositor       ARM64 阶段 5 完整版（应用商店/安装闭环 + 多任务小窗 + 息屏显示 AOD）
#   - /etc/aether/phonetest.sh      阶段 6 自检（v5 全量 + 商店安装闭环 / 小窗 / AOD 断言 + 逐帧证据）
#   - /etc/aether/run-compositor    生产版启动脚本（与 v5 基线一致，日志走串口）
#   - /etc/aether/version           系统版本（1.0.14-stage5）
#   - /data/aether/*.conf           本机真实数据：离线路线 / 联系人 / 书签
#   - /usr/share/aether/wallpapers/wall-02.ppm  浅色壁纸（同心波纹对比度增强，
#     保证浅色主屏「底层清晰壁纸 / 控件区磨砂」分层可见，总纲 6.1 / 6.4）
# 其余 /usr/share/aether/** 片源、音频、深色壁纸原样保留。
#
# 真正的根因（阶段 6 排障结论）：v5 基线盘的 ext4 journal 带 needs_recovery 标志
# （残留未回放事务）。debugfs -w 绕过 journal 直接改盘，之后 guest 内核挂载时的
# journal recovery（或 e2fsck）会把大文件修改回滚成旧版本，并留下 block bitmap /
# orphan inode / 引用计数不一致，表现为 EXT4-fs error / Structure needs cleaning，
# compositor 在 exec 阶段 rc=139/126 —— 此前一度被误判为 CPU/glibc/C 代码问题。
# 修复：注入前先 e2fsck -fy 回放并清空 journal、固化元数据；注入后再 e2fsck -fy/-fn
# 复检；且不得对基线已存在的目录重复 mkdir（会产生 orphan inode）。
set -e

WORK=/workspace/aether-vm/build/work
BASE="/workspace/aether-vm/out/system-v5-rerun/disk.qcow2"
OUT="$WORK/aetheros-system-v6.qcow2"
CANON=/root/aether-archive/aether-aarch64-build/aetheros-system-v6.qcow2
RAW="$WORK/v6.raw"
COMP=/root/aether-archive/aether-aarch64-build/root-full/sbin/aether-compositor
ANDROID=/root/aether-archive/aether-aarch64-build/root-full/sbin/aether-android
ANDROID_INITD=/root/aether-archive/aether-aarch64-build/stage/etc/aether/init.d/aether-android
PHONETEST="/root/aether-archive/aether-aarch64-build/phonetest-v6.sh"
STAGE=/root/aether-archive/aether-aarch64-build/stage
VERSION="$STAGE/etc/aether/version"
WALL_LIGHT="$STAGE/usr/share/aether/wallpapers/wall-02.ppm"
DATA="$STAGE/data/aether"
CMDF="$WORK/debugfs-v6.cmd"

for f in "$BASE" "$COMP" "$ANDROID" "$ANDROID_INITD" "$PHONETEST" "$VERSION" "$WALL_LIGHT" "$STAGE/etc/aether/run-compositor"; do
    [ -f "$f" ] || { echo "缺少 $f"; exit 1; }
done
[ -d "$DATA" ] || { echo "缺少数据目录 $DATA"; exit 1; }
mkdir -p "$WORK" "$(dirname "$CANON")"

# 自检脚本同步到 stage，保证 stage 与系统盘内容一致
cp "$PHONETEST" "$STAGE/etc/aether/phonetest.sh"
chmod 755 "$STAGE/etc/aether/phonetest.sh"

echo "[1/6] qcow2 -> raw，先回放并清空基线 journal（关键前置）"
qemu-img convert -O raw "$BASE" "$RAW"
e2fsck -fy "$RAW" > "$WORK/e2fsck-journal.log" 2>&1 || true
echo "  journal 回放次数: $(grep -c 'recovering journal' "$WORK/e2fsck-journal.log")"
if dumpe2fs -h "$RAW" 2>/dev/null | grep -q needs_recovery; then
    echo "  !! journal 未清空（needs_recovery 仍在），终止"; exit 1
fi
echo "  ok needs_recovery 已清除"
if ! e2fsck -fn "$RAW" > "$WORK/e2fsck-base.log" 2>&1; then
    echo "  !! 基线盘元数据不干净，终止"; tail -20 "$WORK/e2fsck-base.log"; exit 1
fi
echo "  ok 基线干净: $(grep -E 'files \(.*non-contiguous\)' "$WORK/e2fsck-base.log" | head -1)"

echo "[2/6] 生成 debugfs 命令（不重复 mkdir 已存在目录）"
{
    echo "rm /sbin/aether-compositor"
    echo "write $COMP /sbin/aether-compositor"
    echo "sif /sbin/aether-compositor mode 0100755"
    echo "rm /etc/aether/phonetest.sh"
    echo "write $PHONETEST /etc/aether/phonetest.sh"
    echo "sif /etc/aether/phonetest.sh mode 0100755"
    echo "rm /etc/aether/version"
    echo "write $VERSION /etc/aether/version"
    echo "rm /etc/aether/run-compositor"
    echo "write $STAGE/etc/aether/run-compositor /etc/aether/run-compositor"
    echo "sif /etc/aether/run-compositor mode 0100755"
    echo "rm /sbin/aether-android"
    echo "write $ANDROID /sbin/aether-android"
    echo "sif /sbin/aether-android mode 0100755"
    echo "rm /etc/aether/init.d/aether-android"
    echo "write $ANDROID_INITD /etc/aether/init.d/aether-android"
    echo "rm /usr/share/aether/wallpapers/wall-02.ppm"
    echo "write $WALL_LIGHT /usr/share/aether/wallpapers/wall-02.ppm"
    for f in route.conf contacts.conf bookmarks.conf; do
        echo "rm /data/aether/$f"
        echo "write $DATA/$f /data/aether/$f"
    done
} > "$CMDF"

echo "[3/6] 注入 $(grep -c '^write ' "$CMDF") 个文件"
debugfs -w -f "$CMDF" "$RAW" > "$WORK/debugfs-v6.log" 2>&1 || true
grep -iE 'error|failed' "$WORK/debugfs-v6.log" | head -20 || true
echo "  (文件内容由第 5 步 SHA-256 硬校验兜底)"

echo "[4/6] e2fsck -fy 复修并 -fn 复检"
e2fsck -fy "$RAW" > "$WORK/e2fsck-fix.log" 2>&1 || true
grep -E 'MODIFIED|bitmap differences|recovering journal' "$WORK/e2fsck-fix.log" | head -10 || true
if ! e2fsck -fn "$RAW" > "$WORK/e2fsck-clean.log" 2>&1; then
    echo "  !! 修复后仍不干净，终止"; tail -20 "$WORK/e2fsck-clean.log"; exit 1
fi
echo "  ok 复检干净: $(grep -E 'files \(.*non-contiguous\)' "$WORK/e2fsck-clean.log" | head -1)"

echo "[5/6] 内容 SHA-256 + 权限位校验（盘内 dump 对比宿主源文件）"
fail=0
verify() {
    local path="$1" src="$2" want_mode="$3" tmp a b mode
    tmp=$(mktemp); rm -f "$tmp"
    if ! debugfs -R "dump $path $tmp" "$RAW" >/dev/null 2>&1; then
        echo "  !! $path dump 失败"; fail=1; rm -f "$tmp"; return
    fi
    a=$(sha256sum "$src" | awk '{print $1}')
    b=$(sha256sum "$tmp" | awk '{print $1}')
    rm -f "$tmp"
    if [ "$a" != "$b" ]; then
        echo "  !! $path SHA 不一致 host=${a:0:12} disk=${b:0:12}"; fail=1; return
    fi
    if [ -n "$want_mode" ]; then
        mode=$(debugfs -R "stat $path" "$RAW" 2>/dev/null | grep -oE 'Mode:[[:space:]]+[0-9]+' | head -1 | awk '{print $2}')
        if [ "$mode" != "$want_mode" ]; then
            echo "  !! $path 权限 $mode != $want_mode"; fail=1; return
        fi
    fi
    echo "  ok $path  ${a:0:16}…  $(stat -c %s "$src") B  ${want_mode:-[不改权限]}"
}
verify /sbin/aether-compositor      "$COMP"                            0755
verify /sbin/aether-android         "$ANDROID"                         0755
verify /etc/aether/init.d/aether-android "$ANDROID_INITD"               ""
verify /usr/share/aether/wallpapers/wall-02.ppm "$WALL_LIGHT"          ""
verify /etc/aether/phonetest.sh     "$PHONETEST"                       0755
verify /etc/aether/run-compositor   "$STAGE/etc/aether/run-compositor" 0755
verify /etc/aether/version          "$VERSION"                         ""
verify /data/aether/route.conf      "$DATA/route.conf"                 ""
verify /data/aether/contacts.conf   "$DATA/contacts.conf"              ""
verify /data/aether/bookmarks.conf  "$DATA/bookmarks.conf"             ""
[ "$fail" = 0 ] || { echo "内容校验失败，保留 raw 供排查：$RAW"; exit 1; }

echo "[6/6] 回写 qcow2 并同步到 canonical 路径"
qemu-img convert -O qcow2 "$RAW" "$OUT"
cp "$OUT" "$CANON"
echo "  -> $OUT"
echo "  -> $CANON"
echo "  一致性: $(sha256sum "$OUT" "$CANON" | awk '{print $1}' | sort -u | wc -l) 个不同哈希（应为 1）"
rm -f "$RAW"
qemu-img info "$CANON" | sed -n '1,6p'
echo "完成"
