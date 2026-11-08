#!/bin/bash
# make_rootfs_pc.sh - 生成 AetherOS PC 的 ext4 根文件系统镜像
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
# 构建中间产物统一放到其他存储（/root/aether-archive），不污染工作区
BUILD="${AETHER_BUILD:-/root/aether-archive/aether-pc-build}"
STAGE="$BUILD/stage"
BB="$BUILD/busybox/_install"
BIN="$BUILD/root"
IMG="${ROOTFS:-$BUILD/aetheros-rootfs-pc.ext4}"
SIZE="${SIZE:-320}"   # MB

echo "[rootfs] 清理并重建 staging: $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "[rootfs] 复制静态 busybox 基座"
cp -a "$BB/." "$STAGE/"

echo "[rootfs] 复制 AetherOS 用户态"
cp -a "$BIN/." "$STAGE/"

echo "[rootfs] 创建目录结构"
mkdir -p "$STAGE"/{proc,sys,dev,dev/pts,tmp,run,run/aether,mnt/share,data/aether/crash,etc/aether/init.d,etc/aether,var/log}
chmod 1777 "$STAGE/tmp"

echo "[rootfs] 写入基础配置文件"
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

echo "[rootfs] 写入服务配置 (/etc/aether/init.d)"
cat > "$STAGE/etc/aether/init.d/aetherosd" <<'EOF'
name=aetherosd
exec=/sbin/aetherosd
restart=always
EOF

PROFILE="${AETHER_PROFILE:-phone}"
if [ "$PROFILE" = "phone" ]; then
  RES_LINE='export AETHER_RES=720x1280'
  PHONE_LINE='export AETHER_PHONE=1'
else
  RES_LINE='# 桌面形态：不强制竖屏，画布=物理分辨率'
  PHONE_LINE='# 桌面形态：不进入手机外壳'
fi
cat > "$STAGE/etc/aether/run-compositor" <<EOF
#!/bin/sh
# 手机版默认竖屏分辨率；真机由 Limbo 机器分辨率配置提供，此处保持一致。
# 合成器会把手机 UI 渲染到 720x1280 逻辑画布，再缩放进任意物理帧缓冲（DRM/fbdev 均可），
# 因此不依赖特定后端能否设定竖屏模式。桌面形态不设置 AETHER_PHONE，画布=物理分辨率。
$RES_LINE
$PHONE_LINE
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

echo "[rootfs] 写入手机自检脚本（仅 aether.phonetest=1 时启用，9p 共享导出逐帧证据）"
cat > "$STAGE/etc/aether/phonetest.sh" <<'EOF'
#!/bin/sh
# AetherOS 手机自检演示：由内核参数 aether.phonetest=1 触发。
grep -q "aether.phonetest=1" /proc/cmdline 2>/dev/null || exit 0
# 通过 aether-ipc 走与真实点击相同的 handler，把手机各状态渲染帧导出到 9p 共享，
# 供宿主逐帧取证（锁屏/主屏/应用/多任务/通知）。正常开机不会执行本脚本。
I=/bin/aether-ipc
T=/dev/ttyS0
mkdir -p /data/aether /mnt/share
# 优先 9p 共享导出；不可用（部分内核无 virtio-9p）时落到磁盘持久目录 /data/aether，事后从镜像提取
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/share 2>/dev/null
OUT=/data/aether
grep -q " /mnt/share " /proc/mounts 2>/dev/null && OUT=/mnt/share
i=0
while [ $i -lt 40 ] && [ ! -S /run/aetheros-comp.sock ]; do i=$((i+1)); sleep 1; done
[ -S /run/aetheros-comp.sock ] || { echo "PHONETEST: 无 compositor 套接字" > $T; exit 0; }
# 锁屏（3 秒自动解锁前）：合成器就绪有毫秒级抖动，重试直到成功且未过 3 秒
for _t in 1 2 3 4 5 6 7 8; do
    aether-shot "$OUT/phone-0-lock.ppm" 2>/dev/null && { echo "PHONETEST: 锁屏截图 phone-0-lock.ppm -> $OUT" > $T; break; }
    sleep 0.3
done
sleep 4   # 等待锁屏 3s 自动解锁
echo "PHONETEST_OUT=$OUT" > $T
shot(){ aether-shot "$OUT/$1" 2>/dev/null && echo "PHONETEST: 截图 $1 -> $OUT" > $T; }
echo "PHONETEST_STATE=$( $I state 2>/dev/null )" > $T
$I home    >/dev/null 2>&1; shot phone-1-home.ppm
$I open 0  >/dev/null 2>&1; shot phone-2-app0.ppm       # 第一个应用（按 APP 顺序）
$I back    >/dev/null 2>&1
$I open 1  >/dev/null 2>&1; shot phone-3-app1.ppm
$I back    >/dev/null 2>&1
$I open 2  >/dev/null 2>&1; shot phone-4-app2.ppm
$I back    >/dev/null 2>&1
$I recents >/dev/null 2>&1; shot phone-5-recents.ppm
$I home    >/dev/null 2>&1
$I notif   >/dev/null 2>&1; shot phone-6-notif.ppm
$I notif   >/dev/null 2>&1
sync
echo "PHONETEST_DONE=1" > $T
EOF
chmod +x "$STAGE/etc/aether/phonetest.sh"

cat > "$STAGE/etc/aether/init.d/phonetest" <<'EOF'
name=phonetest
exec=/etc/aether/phonetest.sh
depends=aether-compositor
restart=never
EOF

echo "[rootfs] 写入诊断 / 截图脚本"
cat > "$STAGE/etc/aether/bootdiag.sh" <<'EOF'
#!/bin/sh
# 等待关键 IPC 就绪，汇总启动诊断
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
# 桌面自检演示：仅在内核参数含 aether.desktest=1 时启用，正常开机不触发
if grep -q "aether.desktest=1" /proc/cmdline 2>/dev/null; then
  /etc/aether/desktest.sh
fi
EOF
chmod +x "$STAGE/etc/aether/bootdiag.sh"

echo "[rootfs] 写入桌面自检脚本（仅自检模式启用）"
cat > "$STAGE/etc/aether/desktest.sh" <<'EOF'
#!/bin/sh
# AetherOS 桌面自检演示：由内核参数 aether.desktest=1 触发。
# 通过 aether-ipc 走与真实点击相同的 handler，每步把桌面状态 JSON 打到串口，
# 供宿主逐帧抓屏取证。正常开机不会执行本脚本。
I=/bin/aether-ipc
T=/dev/ttyS0
step(){
  ST=$($I state 2>/dev/null)
  echo "DESKTEST_STEP=$1 STATE=$ST" > $T
  sleep 2
}
i=0
while [ $i -lt 40 ] && [ ! -S /run/aetheros-comp.sock ]; do i=$((i+1)); sleep 1; done
$I skipboot >/dev/null 2>&1
$I closeall >/dev/null 2>&1
sleep 1

step 1                          # 初始桌面：图标 + 任务栏
$I click 60 776 >/dev/null 2>&1   # 开始按钮
step 2                          # 开始菜单展开
$I click 100 452 >/dev/null 2>&1  # 菜单里的「终端」
step 3                          # 终端窗口打开
$I click 176 471 >/dev/null 2>&1  # 终端快捷命令「uname -a」
step 4                          # 真实命令输出
$I closeall >/dev/null 2>&1
$I click 64 94 >/dev/null 2>&1    # 桌面图标「文件管理器」
step 5
$I closeall >/dev/null 2>&1
$I click 64 382 >/dev/null 2>&1   # 桌面图标「计算器」
step 6
$I click 168 262 >/dev/null 2>&1  # 7
$I click 411 318 >/dev/null 2>&1  # +
$I click 249 262 >/dev/null 2>&1  # 8
$I click 411 374 >/dev/null 2>&1  # =
step 7                          # 7+8=15
$I closeall >/dev/null 2>&1
$I click 164 382 >/dev/null 2>&1  # 桌面图标「系统模块清单」
step 8
$I click 200 305 >/dev/null 2>&1  # 选中第九类「图形显示与桌面环境」
step 9
$I closeall >/dev/null 2>&1
$I click 64 190 >/dev/null 2>&1   # 桌面图标「终端」
step 10                          # 终端窗口基线 x=120,y=74
$I down 300 88 >/dev/null 2>&1   # 按住标题栏
$I move 450 160 >/dev/null 2>&1  # 拖动窗口
$I up 450 160 >/dev/null 2>&1
step 11                          # 窗口已移动
$I click 972 161 >/dev/null 2>&1 # 标题栏关闭按钮
step 12                          # 窗口已关闭
echo "DESKTEST_DONE=1" > $T
EOF
chmod +x "$STAGE/etc/aether/desktest.sh"

cat > "$STAGE/etc/aether/pcshot.sh" <<'EOF'
#!/bin/sh
# 首屏截图：有 9p 共享就导出到宿主机（桌面 QEMU 验证用），
# 没有 9p（Limbo / 真机）就只留在 /run/aether，绝不阻塞开机。
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

echo "[rootfs] 生成 ext4 镜像 (mke2fs -d, ${SIZE}MB): $IMG"
rm -f "$IMG"
mkdir -p "$(dirname "$IMG")"
mke2fs -q -d "$STAGE" -t ext4 -b 4096 -L aetheros -O ^metadata_csum "$IMG" "${SIZE}M"
echo "[rootfs] 完成: $IMG"
ls -la "$IMG"
