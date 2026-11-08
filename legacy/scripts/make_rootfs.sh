#!/bin/bash
# 构建 AetherOS 真实 rootfs (ext4) —— 替代 initramfs，挂载为 virtio 磁盘
#
# 可复现性（CI 2026-10-18）：所有输入路径均可用环境变量覆盖，
# 默认值指向本仓库相对位置，使 CI 与本地沙箱跑同一条构建路径：
#   BB          busybox _install 树（交叉编译产物）
#   ALIB        交叉编译器 sysroot 的 lib 目录（动态库来源）
#   ROOT        rootfs 暂存目录
#   IMG         输出 ext4 镜像
#   AETHER_INIT aether-init aarch64 二进制
#   AETHEROSD   aetherosd aarch64 二进制
#   AETHER_COMP aether-compositor aarch64 二进制（P0-6 图形栈）
#   AETHER_OTA  aether-ota aarch64 二进制（P1-2c A/B 双槽 OTA）
#   AETHER_PKG_HOST  宿主 aether-pkg 二进制（P2-11：给 rootfs 里的 demo 签名打包）
#   AETHER_PKG       客机 aether-pkg aarch64 二进制（P2-11 包管理器）
#   AETHER_SANDBOX_RUN 客机 aether-sandbox-run aarch64（P2-11 应用运行时入口）
#   AETHER_SETTINGS 客机 aether-settings aarch64（P4-1「设置」应用）
#   AETHER_CLOCK    客机 aether-clock aarch64（P4-1「时钟」应用）
#   AETHER_UPDATER  客机 aether-updater aarch64（P5「系统更新」应用）
#   AETHER_SIGN_KEY .aepkg 发布签名密钥（宿主侧；见 ci/test-signing.key 头注释）
#   SLOT        A|B —— 写入 /etc/aether-slot 的槽位标记（P1-2c A/B）；
#               同一个系统镜像装到两个槽时靠它自证本次启动的是哪个槽
set -e
REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BB=${BB:-/tmp/linux-build/busybox-1.36.1/_install}
ALIB=${ALIB:-/usr/aarch64-linux-gnu/lib}
ROOT=${ROOT:-/tmp/linux-build/rootfs}
IMG=${IMG:-/tmp/linux-build/aetheros-rootfs.ext4}
AETHER_INIT=${AETHER_INIT:-$REPO_ROOT/aether-init-aarch64}
AETHEROSD=${AETHEROSD:-$REPO_ROOT/aetherosd-aarch64-static}
AETHER_COMP=${AETHER_COMP:-$REPO_ROOT/aether-compositor}
AETHER_DEMO=${AETHER_DEMO:-$REPO_ROOT/aether-demo}
AETHER_OTA=${AETHER_OTA:-$REPO_ROOT/aether-ota-aarch64}
AETHER_PKG_HOST=${AETHER_PKG_HOST:-$REPO_ROOT/build/aether-pkg-host}
AETHER_PKG=${AETHER_PKG:-$REPO_ROOT/build/aether-pkg-aarch64}
AETHER_SANDBOX_RUN=${AETHER_SANDBOX_RUN:-$REPO_ROOT/build/aether-sandbox-run}
AETHER_SETTINGS=${AETHER_SETTINGS:-$REPO_ROOT/build/aether-settings}       # P4-1「设置」应用
AETHER_CLOCK=${AETHER_CLOCK:-$REPO_ROOT/build/aether-clock}               # P4-1「时钟」应用
AETHER_UPDATER=${AETHER_UPDATER:-$REPO_ROOT/build/aether-updater}         # P5「系统更新」应用
AETHER_LOGD=${AETHER_LOGD:-$REPO_ROOT/build/aether-logd}                 # P2-13a 序列化日志服务
AETHER_LOGCTL=${AETHER_LOGCTL:-$REPO_ROOT/build/aether-logctl}           # P2-13a 日志手工/测试客户端
AETHER_CRASHD=${AETHER_CRASHD:-$REPO_ROOT/build/aether-crashd}           # P2-13b 崩溃管理器
AETHER_WATCHDOG=${AETHER_WATCHDOG:-$REPO_ROOT/build/aether-watchdog}     # P2-13c 软看门狗
AETHER_SYSUI=${AETHER_SYSUI:-$REPO_ROOT/build/aether-systemui}           # P3-14 SystemUI 服务
AETHER_SYSUICTL=${AETHER_SYSUICTL:-$REPO_ROOT/build/aether-sysuictl}     # P3-14 SystemUI 客户端
AETHER_HAL_ANDROID=${AETHER_HAL_ANDROID:-$REPO_ROOT/build/aether-hal-android}  # Android 宿主 HAL 后端
AETHER_SIGN_KEY=${AETHER_SIGN_KEY:-$REPO_ROOT/ci/test-signing.key}
SLOT=${SLOT:-A}
[ -x "$AETHER_PKG_HOST" ] || { echo "FATAL: AETHER_PKG_HOST ($AETHER_PKG_HOST) 不存在——先用宿主工具链构建 aether-pkg" >&2; exit 2; }
[ -x "$AETHER_PKG" ] || { echo "FATAL: AETHER_PKG ($AETHER_PKG) 不存在——先交叉编译 aether-pkg" >&2; exit 2; }
[ -x "$AETHER_SANDBOX_RUN" ] || { echo "FATAL: AETHER_SANDBOX_RUN ($AETHER_SANDBOX_RUN) 不存在——先交叉编译 aether-sandbox-run" >&2; exit 2; }
[ -x "$AETHER_LOGD" ] || { echo "FATAL: AETHER_LOGD ($AETHER_LOGD) 不存在——先交叉编译 aether-logd" >&2; exit 2; }
[ -x "$AETHER_LOGCTL" ] || { echo "FATAL: AETHER_LOGCTL ($AETHER_LOGCTL) 不存在——先交叉编译 aether-logctl" >&2; exit 2; }
[ -x "$AETHER_CRASHD" ] || { echo "FATAL: AETHER_CRASHD ($AETHER_CRASHD) 不存在——先交叉编译 aether-crashd" >&2; exit 2; }
[ -x "$AETHER_WATCHDOG" ] || { echo "FATAL: AETHER_WATCHDOG ($AETHER_WATCHDOG) 不存在——先交叉编译 aether-watchdog" >&2; exit 2; }
[ -x "$AETHER_SYSUI" ] || { echo "FATAL: AETHER_SYSUI ($AETHER_SYSUI) 不存在——先交叉编译 aether-systemui" >&2; exit 2; }
[ -x "$AETHER_SYSUICTL" ] || { echo "FATAL: AETHER_SYSUICTL ($AETHER_SYSUICTL) 不存在——先交叉编译 aether-sysuictl" >&2; exit 2; }
[ -x "$AETHER_SETTINGS" ] || { echo "FATAL: AETHER_SETTINGS ($AETHER_SETTINGS) 不存在——先交叉编译 aether-settings" >&2; exit 2; }
[ -x "$AETHER_CLOCK" ] || { echo "FATAL: AETHER_CLOCK ($AETHER_CLOCK) 不存在——先交叉编译 aether-clock" >&2; exit 2; }
[ -x "$AETHER_UPDATER" ] || { echo "FATAL: AETHER_UPDATER ($AETHER_UPDATER) 不存在——先交叉编译 aether-updater" >&2; exit 2; }
case "$SLOT" in
  A|B) ;;
  *) echo "FATAL: SLOT must be A or B (got '$SLOT')" >&2; exit 2 ;;
esac

rm -rf "$ROOT" && mkdir -p "$ROOT"

# 1) 复制 busybox 安装树(含 bin/sbin/usr/linuxrc 等)
cp -a "$BB"/. "$ROOT"/

# 2) 标准目录（含 P2-11 的应用数据区：/data/aether/{packages,apps}）
mkdir -p "$ROOT"/{proc,sys,dev/pts,tmp,var/log,var/run,root,home,usr,opt,mnt,run,media,etc/init.d,etc/aether,data/aether/packages,data/aether/apps}

# 3) 动态链接库(从交叉编译器 sysroot)
mkdir -p "$ROOT/lib"
for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libresolv.so.2 libgcc_s.so.1 libdl.so.2 libpthread.so.0; do
  cp -aL "$ALIB/$lib" "$ROOT/lib/" 2>/dev/null || true
done

# 4) PID 1 = aether-init（AetherOS 原生 init，替代 busybox init）
#    源码见 aether-init.c；busybox 仅作为用户态工具集（sh/getty/mount/ps 等）保留。
#    /sbin/init 用软链而非拷贝：ps 中 comm=aether-init，与 busybox init(comm=init)
#    明确区分，运行时可自证"PID 1 是 AetherOS 原生 init"。
cp -a "$AETHER_INIT" "$ROOT/sbin/aether-init"
chmod 755 "$ROOT/sbin/aether-init"
rm -f "$ROOT/sbin/init" "$ROOT/init"
ln -sf aether-init "$ROOT/sbin/init"              # 内核 exec 的 /sbin/init
ln -sf sbin/init "$ROOT/init"                     # 兼容 /init 路径

# 5) AetherOS 原生系统守护进程 + IPC 客户端(cross-compiled aarch64 static ELF)
cp -a "$AETHEROSD" "$ROOT/sbin/aetherosd"
chmod 755 "$ROOT/sbin/aetherosd"
ln -sf /sbin/aetherosd "$ROOT/bin/aetherctl"

# 5a) 合成器（P0-6）：DRM/KMS 显示 + 窗口管理 + IPC
#    aethercompctl 软链模式与 aetherctl 一致：-c 走客户端模式
cp -a "$AETHER_COMP" "$ROOT/sbin/aether-compositor"
chmod 755 "$ROOT/sbin/aether-compositor"
ln -sf /sbin/aether-compositor "$ROOT/bin/aethercompctl"

# 5b) 示例原生应用（P1）：独立进程经 IPC + SCM_RIGHTS 共享内存把像素上屏，
#     证明合成器是真实多进程窗口系统，而非自绘两个固定窗口。
cp -a "$AETHER_DEMO" "$ROOT/sbin/aether-demo"
chmod 755 "$ROOT/sbin/aether-demo"

# 5c) OTA 槽位管理器（P1-2c）：非 root 手动调用（或后续由更新服务调用），
#     自行挂载 /dev/vdc 持久分区并翻转槽位标记；翻转前先校验目标槽。
cp -a "$AETHER_OTA" "$ROOT/sbin/aether-ota"
chmod 755 "$ROOT/sbin/aether-ota"
ln -sf /sbin/aether-ota "$ROOT/bin/aether-ota"

# 5e) 包管理器 + 应用运行时入口（P2-11）：aether-pkg 管 .aepkg 的
#     验签/安装/索引；aether-sandbox-run 给安装的应用施加与 init 服务
#     沙箱相同的隔离后 exec（沙箱失败即拒跑，不静默降级）。
cp -a "$AETHER_PKG" "$ROOT/sbin/aether-pkg"
chmod 755 "$ROOT/sbin/aether-pkg"
ln -sf /sbin/aether-pkg "$ROOT/bin/aether-pkg"
cp -a "$AETHER_SANDBOX_RUN" "$ROOT/sbin/aether-sandbox-run"
chmod 755 "$ROOT/sbin/aether-sandbox-run"

# 5g) 可观测性三件套（P2-13）：aether-logd 序列化日志服务 +
#     aether-crashd 崩溃管理器 + aether-watchdog 软看门狗。
#     logd 最先启动（其余服务的日志才有主路径）；crashd 紧随
#     （init 的异常退出事件需要听众）；watchdog 依赖两个被监控服务。
for b in "$AETHER_LOGD" "$AETHER_LOGCTL" "$AETHER_CRASHD" "$AETHER_WATCHDOG"; do
    cp -a "$b" "$ROOT/sbin/$(basename "$b")"
    chmod 755 "$ROOT/sbin/$(basename "$b")"
    ln -sf "/sbin/$(basename "$b")" "$ROOT/bin/$(basename "$b")"
done
mkdir -p "$ROOT/data/aether/crash"   # crashd 报告目录（/data 持久分区）

# 5h) SystemUI（P3-14）：状态条 + Launcher。与 demo 一样是 compositor 的
#     客户端进程——经 UDS 注册窗口、SCM_RIGHTS 共享像素、自绘上屏。
#     以系统服务运行（非沙箱）：它需要 fork/exec 沙箱启动器来启动应用。
for b in "$AETHER_SYSUI" "$AETHER_SYSUICTL"; do
    cp -a "$b" "$ROOT/sbin/$(basename "$b")"
    chmod 755 "$ROOT/sbin/$(basename "$b")"
    ln -sf "/sbin/$(basename "$b")" "$ROOT/bin/$(basename "$b")"
done

# 5i) Android 宿主 HAL 后端（客户机侧）：轮询 9p 共享里的 android-host.json
#     快照，落盘 /run/aether-hal-android.state 供 SystemUI 消费。通道用 9p
#     文件落盘（零额外依赖）。桥接未运行/文件缺失时写 backend=NONE（诚实）。
cp -a "$AETHER_HAL_ANDROID" "$ROOT/sbin/aether-hal-android"
chmod 755 "$ROOT/sbin/aether-hal-android"
ln -sf /sbin/aether-hal-android "$ROOT/bin/aether-hal-android"

# 5f) 把三个应用打成 .aepkg（宿主侧用发布密钥签名，构建期自验签），
#     随 rootfs 发布；开机由 pkginstall 服务真实安装（见下方服务清单）。
#     ——这是 AetherOS 第一批「签名打包发布」的应用，安装走与运行时
#     安装完全相同的 aether-pkg install 路径（一个循环装全部）。
AETHER_PKG_PRIVKEY="$AETHER_SIGN_KEY" "$AETHER_PKG_HOST" create \
    "$ROOT/data/aether/packages/demo.aepkg" \
    aetheros.demo "AetherOS Demo" 1.0.0 demo "$AETHER_DEMO=demo" \
    || { echo "FATAL: demo.aepkg 签名打包失败" >&2; exit 3; }
AETHER_PKG_PRIVKEY="$AETHER_SIGN_KEY" "$AETHER_PKG_HOST" create \
    "$ROOT/data/aether/packages/settings.aepkg" \
    aetheros.settings "AetherOS Settings" 1.0.0 settings \
    "$AETHER_SETTINGS=settings" \
    || { echo "FATAL: settings.aepkg 签名打包失败" >&2; exit 3; }
AETHER_PKG_PRIVKEY="$AETHER_SIGN_KEY" "$AETHER_PKG_HOST" create \
    "$ROOT/data/aether/packages/clock.aepkg" \
    aetheros.clock "AetherOS Clock" 1.0.0 clock "$AETHER_CLOCK=clock" \
    || { echo "FATAL: clock.aepkg 签名打包失败" >&2; exit 3; }
# P5:「系统更新」应用（与设置/时钟同一安装路径；客机内经 10.0.2.2 访问宿主）
AETHER_PKG_PRIVKEY="$AETHER_SIGN_KEY" "$AETHER_PKG_HOST" create \
    "$ROOT/data/aether/packages/updater.aepkg" \
    aetheros.updater "AetherOS Update" 1.0.0 updater "$AETHER_UPDATER=updater" \
    || { echo "FATAL: updater.aepkg 签名打包失败" >&2; exit 3; }

# 5g) 壁纸（手机产品形态：真照片底图，玻璃的模糊/折射/色差才有内容可
#     扭曲）。PPM P6 720x1600 = 竖屏 1080x2400 手机 9:20 的原生比例；
#     合成器按 cover 采样铺满任意 KMS 模式。缺失时合成器如实回退渐变。
AETHER_WALLPAPER_SRC=${AETHER_WALLPAPER_SRC:-$REPO_ROOT/assets/wallpaper-720x1600.ppm}
mkdir -p "$ROOT/usr/share/aether"
if [ -f "$AETHER_WALLPAPER_SRC" ]; then
  cp "$AETHER_WALLPAPER_SRC" "$ROOT/usr/share/aether/wallpaper.ppm"
else
  echo "WARN: 壁纸 $AETHER_WALLPAPER_SRC 缺失 —— 桌面将回退渐变" >&2
fi

# 5d) 槽位标记（P1-2c）：自证本次启动的是哪个槽的系统镜像。
#     与 initramfs 选择器 (ci/ab-initramfs-init.sh) 读的 /persist/slot 相互印证：
#     选择器按 persist 标记挂盘，本文件由该盘内容自证身份。
printf '%s\n' "$SLOT" > "$ROOT/etc/aether-slot"

# 5d-2) 镜像版本标记（P5）：宿主 APK 装完读它自报真实版本——比"从下载 URL
#       猜 tag"可靠（镜像站重定向会丢路径）。checkUpdate 拿它与 GitHub
#       latest tag 精确比较，装的是最新版时不误报"有更新"。
printf '%s\n' "${AETHER_VERSION:-phone-v8}" > "$ROOT/etc/aether-version"

# 5) 系统账户与基础配置
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
daemon:x:1:1:daemon:/usr/sbin:/bin/false
bin:x:2:2:bin:/bin:/bin/false
nobody:x:65534:65534:nobody:/nonexistent:/bin/false
aether:x:1000:1000:aether app:/nonexistent:/bin/false
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
daemon:x:1:
bin:x:2:
nobody:x:65534:
aether:x:1000:
EOF
cat > "$ROOT/etc/shadow" <<'EOF'
root::0:0:99999:7:::
EOF
printf 'aetheros\n' > "$ROOT/etc/hostname"
cat > "$ROOT/etc/os-release" <<'EOF'
NAME="AetherOS Linux"
ID=aetheros
VERSION_ID="1.0.10"
PRETTY_NAME="AetherOS Linux (native Linux kernel userland)"
EOF
cat > "$ROOT/etc/resolv.conf" <<'EOF'
nameserver 8.8.8.8
nameserver 1.1.1.1
EOF
cat > "$ROOT/etc/fstab" <<'EOF'
proc            /proc   proc    defaults        0 0
sysfs           /sys    sysfs   defaults        0 0
devtmpfs        /dev    devtmpfs defaults        0 0
tmpfs           /tmp    tmpfs   defaults        0 0
tmpfs           /run    tmpfs   defaults        0 0
EOF

# 6) AetherOS 服务清单（由 aether-init 解析，PID 1 的服务监管配置）
#    依赖拓扑：network(oneshot) -> aetherosd(daemon) -> compositor(daemon)
#              -> systemui(daemon) -> bootdiag(oneshot) -> getty(daemon)
#    pkginstall(oneshot)：开机真实执行一次 /etc/init.d/aether-pkginstall，
#    循环安装 /data/aether/packages/ 下全部签名包（demo/settings/clock/updater），
#    与运行时手动安装走同一条验签+解包+索引路径。
#    P4-1 起 demo 不再是开机服务：开机只见 SystemUI 锁屏，应用只由用户
#    从启动器点开（launch_app -> aether-sandbox-run）。
cat > "$ROOT/etc/aether/services" <<'EOF'
# AetherOS 服务清单 —— 由 aether-init(PID 1) 解析
# 语法: service <name> / exec <cmdline> / depends a[,b] / restart always|no / oneshot yes|no
service logd
exec /sbin/aether-logd
restart always

service crashd
exec /sbin/aether-crashd
restart always

service network
exec /etc/init.d/aether-net
oneshot yes

service aetherosd
exec /sbin/aetherosd
restart always

service compositor
exec /sbin/aether-compositor
depends aetherosd
restart always

service watchdog
exec /sbin/aether-watchdog
depends aetherosd,compositor
restart always

service pkginstall
exec /etc/init.d/aether-pkginstall
oneshot yes

service systemui
exec /sbin/aether-systemui
depends compositor,pkginstall
restart always

service halandroid
exec /sbin/aether-hal-android
restart always

service bootdiag
exec /etc/init.d/aether-bootdiag
depends network,aetherosd,compositor
oneshot yes

service getty
exec /sbin/getty 115200 /dev/ttyAMA0
depends bootdiag
restart always
EOF

# 6a) 网络与 9p 共享（oneshot：对应原 rcS 中的网络部分）
cat > "$ROOT/etc/init.d/aether-net" <<'EOF'
#!/bin/sh
# aether-net —— virtio 网络 DHCP + 9p 宿主共享（AetherOS 服务清单中的 oneshot）
ifconfig eth0 up 2>/dev/null
if udhcpc -i eth0 -n -q 2>/dev/null; then
  echo "network: eth0 DHCP ok ($(ifconfig eth0 2>/dev/null | grep 'inet addr' | awk '{print $2}'))"
else
  echo "network: eth0 DHCP skipped"
fi
mkdir -p /mnt/share
if mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/share 2>/dev/null; then
  echo "9p: hostshare mounted at /mnt/share"
else
  echo "9p: share not mounted (optional / host did not export)"
fi
EOF
chmod +x "$ROOT/etc/init.d/aether-net"

# 6a-2) 开机安装全部签名应用包（oneshot）：循环 /data/aether/packages/*.aepkg，
#       每个包都走与运行时安装完全相同的 aether-pkg install 验签+解包+索引
#       路径。单个包失败不中断其余包的安装（如实报告，由后续测试断言）。
cat > "$ROOT/etc/init.d/aether-pkginstall" <<'EOF'
#!/bin/sh
# aether-pkginstall —— 开机安装全部签名应用包（P4-1/P5：demo/settings/clock/updater）
for p in /data/aether/packages/*.aepkg; do
  [ -e "$p" ] || continue
  echo "pkginstall: installing $p"
  /sbin/aether-pkg install "$p" || echo "pkginstall: FAILED $p"
done
echo "pkginstall: done"
EOF
chmod +x "$ROOT/etc/init.d/aether-pkginstall"

# 6b) 启动自检（oneshot，依赖 network+aetherosd+compositor：验证 PID 1、服务与 IPC 全链路）
cat > "$ROOT/etc/init.d/aether-bootdiag" <<'EOF'
#!/bin/sh
# aether-bootdiag —— 串口取证：PID 1 身份 / 服务表 / IPC 回环 / 图形栈
echo "[DIAG] pid1      : pid=1 comm=$(cat /proc/1/comm)"
echo "[DIAG] processes :"
ps -o pid,ppid,stat,comm 2>/dev/null
echo "[DIAG] ipc socket: $(ls -la /run/aetheros.sock 2>/dev/null | awk '{print $1}')"
echo "[DIAG] ipc round-trip (aetherctl -> bus):"
aetherctl -c 2>&1
# depends 只保证启动顺序、不保证就绪：合成器建 socket 需要 DRM 初始化时间，
# 这里做有界等待（最多 10s），等待本身也是真实系统行为的取证。
i=0
while [ $i -lt 10 ]; do
  [ -S /run/aetheros-comp.sock ] && break
  i=$((i+1)); sleep 1
done
echo "[DIAG] drm node  : $(ls -la /dev/dri/card0 2>/dev/null | awk '{print $1, $NF}')"
echo "[DIAG] comp socket: $(ls -la /run/aetheros-comp.sock 2>/dev/null | awk '{print $1}') (waited ${i}s)"
echo "[DIAG] comp status (aethercompctl -> compositor):"
aethercompctl -c status 2>&1
echo "[DIAG] pkg manager (aether-pkg list):"
aether-pkg list 2>&1
echo "[DIAG] AETHEROS_BOOT_DIAG_DONE"
EOF
chmod +x "$ROOT/etc/init.d/aether-bootdiag"

# 6c) aether-power：经信号让 aether-init 优雅停机/重启/恢复（不直接调 reboot 绕过监管）
cat > "$ROOT/bin/aether-power" <<'EOF'
#!/bin/sh
# aether-power poweroff|reboot|recovery —— 向 PID 1 发信号，由 aether-init 优雅停机
case "$1" in
  poweroff) kill -TERM 1; echo "aether-power: SIGTERM -> init (poweroff)" ;;
  reboot)   kill -USR1 1; echo "aether-power: SIGUSR1 -> init (reboot)" ;;
  recovery) kill -HUP  1; echo "aether-power: SIGHUP  -> init (recovery)" ;;
  *) echo "usage: aether-power poweroff|reboot|recovery" >&2; exit 2 ;;
esac
EOF
chmod +x "$ROOT/bin/aether-power"

# 7) busybox inittab 已废弃（PID 1 现为 aether-init）；保留文件为空并注明，
#    避免误以为 busybox init 仍在工作。
cat > "$ROOT/etc/inittab" <<'EOF'
# AetherOS: PID 1 is aether-init (native, /sbin/init).
# busybox inittab is no longer used — service policy lives in /etc/aether/services.
EOF

# 8) 打包为 ext4 镜像(-d 直接导入目录,无需 loop 挂载)
rm -f "$IMG"
mkfs.ext4 -F -q -L aetheros-root -b 4096 -d "$ROOT" "$IMG" 512M
echo "ROOTFS_BUILD_OK"
ls -la "$IMG"
