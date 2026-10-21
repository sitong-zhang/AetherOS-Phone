#!/bin/bash
# run-ci.sh —— 在无 GitHub 的环境（沙箱/开发机）完整模拟两条工作流
#
# 模拟范围：
#   [aether-core  CI]   ./gradlew build（wrapper + JDK 20 + 全量测试）
#   [aether-linux build] 交叉编译 -> check-artifacts -> 宿主 IPC 契约冒烟
#   [aether-linux boot ] busybox -> rootfs -> QEMU 启动断言链
#
# 与工作流的差异（仅此一处，如实说明）：CI 在 checkout 工作区内编译产物，
# 本脚本编译到 BUILD_DIR（默认 /tmp/aether-ci-build），不覆盖仓库内预编译二进制。
#
# 用法: run-ci.sh [--skip-core] [--skip-boot]
# 环境: BUILD_DIR / EVIDENCE / BB 等均可覆盖
# 退出: 0 = 全绿; 1 = 存在失败阶段
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-/tmp/aether-ci-build}"
EVIDENCE="${EVIDENCE:-/tmp/aether-ci-evidence.log}"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
SKIP_CORE=0
SKIP_BOOT=0
for arg in "$@"; do
  case "$arg" in
    --skip-core) SKIP_CORE=1 ;;
    --skip-boot) SKIP_BOOT=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

mkdir -p "$BUILD_DIR"
: > "$EVIDENCE"

log() { echo "$*" | tee -a "$EVIDENCE"; }
stage() { log ""; log "========== [$1] $2 =========="; }

FAILED_STAGES=()
run_stage() { # run_stage <名> <命令...>
  local name="$1"; shift
  stage "$name" "$*"
  if "$@" >>"$EVIDENCE" 2>&1; then
    log "[$name] OK"
  else
    local rc=$?
    log "[$name] FAILED (rc=$rc)"
    FAILED_STAGES+=("$name")
    return "$rc"
  fi
}

log "AetherOS 本地 CI 模拟 —— $(date -u +%FT%TZ)"
log "REPO_ROOT=$REPO_ROOT  BUILD_DIR=$BUILD_DIR"

# ---------- 1. aether-core：wrapper + JDK20 + 全量构建测试 ----------
if [ "$SKIP_CORE" -eq 1 ]; then
  log "[core] 跳过（--skip-core）"
elif [ -d "$REPO_ROOT/../aether-core" ]; then
  run_stage core bash -c "cd '$REPO_ROOT/../aether-core' && ./gradlew clean build --stacktrace"
else
  log "[core] 跳过（未找到 ../aether-core）"
fi

# ---------- 2. 交叉编译（与 build.yml 同命令，输出到 BUILD_DIR） ----------
# P2-13a: init 链接 aether-log.c（序列化日志客户端库）
run_stage cross-init "$CROSS"gcc -static -O2 -Wall -I"$REPO_ROOT" -o "$BUILD_DIR/aether-init-aarch64" "$REPO_ROOT/aether-init.c" "$REPO_ROOT/aether-sandbox.c" "$REPO_ROOT/aether-log.c"
run_stage cross-daemon "$CROSS"gcc -static -O2 -Wall -DAETHEROS_BUILD='"ci"' \
  -o "$BUILD_DIR/aetherosd-aarch64-static" "$REPO_ROOT/aetherosd.c"
run_stage cross-compositor "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-compositor" \
  "$REPO_ROOT/aether-compositor.c" "$REPO_ROOT/aether-drm.c" "$REPO_ROOT/aether-render.c"
run_stage cross-demo "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-demo" "$REPO_ROOT/aether-demo.c"
# P1-2c: OTA 槽位管理器（aarch64 静态；槽内 rootfs 与 initramfs 共用）
run_stage cross-ota "$CROSS"gcc -static -O2 -Wall \
  -o "$BUILD_DIR/aether-ota-aarch64" "$REPO_ROOT/aether-ota.c"
# P2-11: 包管理器（客机 aarch64 静态）+ 宿主签名/打包器（同源，跑在 CI 机器上）
# tweetnacl 上游自带 sign-compare 警告，单独以 -w 编译该文件；
# 本仓库源码（aether-pkg.c / aether-sha256.c）仍 -Wall -Wextra 零警告。
run_stage cross-pkg bash -c "
  '$CROSS'gcc -static -O2 -Wall -Wextra -I'$REPO_ROOT' -I'$REPO_ROOT/third_party/tweetnacl' \
    -c '$REPO_ROOT/aether-pkg.c' -o '$BUILD_DIR/aether-pkg-cross.o' &&
  '$CROSS'gcc -static -O2 -Wall -Wextra -I'$REPO_ROOT' \
    -c '$REPO_ROOT/aether-sha256.c' -o '$BUILD_DIR/aether-sha256-cross.o' &&
  '$CROSS'gcc -static -O2 -w -I'$REPO_ROOT/third_party/tweetnacl' \
    -c '$REPO_ROOT/third_party/tweetnacl/tweetnacl.c' -o '$BUILD_DIR/tweetnacl-cross.o' &&
  '$CROSS'gcc -static -o '$BUILD_DIR/aether-pkg-aarch64' \
    '$BUILD_DIR/aether-pkg-cross.o' '$BUILD_DIR/aether-sha256-cross.o' '$BUILD_DIR/tweetnacl-cross.o'"
run_stage cross-sandbox-run "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-sandbox-run" \
  "$REPO_ROOT/aether-sandbox-run.c" "$REPO_ROOT/aether-sandbox.c"
# P2-11: 运行时安装测试应用（打成的 hello.aepkg 由 boot-smoke P 段真实安装）
run_stage cross-hello "$CROSS"gcc -static -O2 -Wall -Wextra \
  -o "$BUILD_DIR/aether-hello" "$REPO_ROOT/aether-hello.c"
# P2-13: 可观测性三件套 + 日志客户端（全部 aarch64 静态）
run_stage cross-logd "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-logd" "$REPO_ROOT/aether-logd.c"
run_stage cross-logctl "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-logctl" "$REPO_ROOT/aether-logctl.c" "$REPO_ROOT/aether-log.c"
run_stage cross-crashd "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-crashd" "$REPO_ROOT/aether-crashd.c"
run_stage cross-watchdog "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-watchdog" "$REPO_ROOT/aether-watchdog.c" "$REPO_ROOT/aether-log.c"
# P3-14: SystemUI（状态条 + Launcher，链接渲染层与日志客户端）
run_stage cross-systemui "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-systemui" "$REPO_ROOT/aether-systemui.c" \
  "$REPO_ROOT/aether-render.c" "$REPO_ROOT/aether-log.c"
run_stage cross-sysuictl "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-sysuictl" "$REPO_ROOT/aether-sysuictl.c"
# P4-1: 「设置」「时钟」两个签名应用（普通窗口客户端，链接渲染层）
run_stage cross-settings "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-settings" "$REPO_ROOT/aether-settings.c" \
  "$REPO_ROOT/aether-render.c"
run_stage cross-clock "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-clock" "$REPO_ROOT/aether-clock.c" \
  "$REPO_ROOT/aether-render.c"
# Android 宿主 HAL 后端（客户机侧）：读 9p 快照落盘供 SystemUI 消费
run_stage cross-hal-android "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-hal-android" "$REPO_ROOT/aether-hal-android.c"
# P5: 「系统更新」应用（普通窗口 + tap 转发 sock=，HTTP 访问宿主 10.0.2.2）
run_stage cross-updater "$CROSS"gcc -static -O2 -Wall -Wextra -I"$REPO_ROOT" \
  -o "$BUILD_DIR/aether-updater" "$REPO_ROOT/aether-updater.c" \
  "$REPO_ROOT/aether-render.c"
# 宿主 aether-pkg：给 rootfs 里的 demo.aepkg 签名（tweetnacl 上游自带
# sign-compare 警告，单独以 -w 编译该文件，本仓库源码仍 -Wall -Wextra 零警告）
run_stage host-pkg bash -c "
  gcc -O2 -Wall -Wextra -I'$REPO_ROOT' -I'$REPO_ROOT/third_party/tweetnacl' -c '$REPO_ROOT/aether-pkg.c' -o '$BUILD_DIR/aether-pkg-main.o' &&
  gcc -O2 -Wall -Wextra -I'$REPO_ROOT' -c '$REPO_ROOT/aether-sha256.c' -o '$BUILD_DIR/aether-sha256.o' &&
  gcc -O2 -w -I'$REPO_ROOT/third_party/tweetnacl' -c '$REPO_ROOT/third_party/tweetnacl/tweetnacl.c' -o '$BUILD_DIR/tweetnacl.o' &&
  gcc -o '$BUILD_DIR/aether-pkg-host' '$BUILD_DIR/aether-pkg-main.o' '$BUILD_DIR/aether-sha256.o' '$BUILD_DIR/tweetnacl.o'"

# ---------- 3. 工件断言 ----------
run_stage artifacts bash "$REPO_ROOT/ci/check-artifacts.sh" \
  "$BUILD_DIR/aether-init-aarch64" "$BUILD_DIR/aetherosd-aarch64-static" \
  "$BUILD_DIR/aether-compositor" "$BUILD_DIR/aether-demo" \
  "$BUILD_DIR/aether-ota-aarch64" "$BUILD_DIR/aether-pkg-aarch64" \
  "$BUILD_DIR/aether-sandbox-run" \
  "$BUILD_DIR/aether-logd" "$BUILD_DIR/aether-logctl" \
  "$BUILD_DIR/aether-crashd" "$BUILD_DIR/aether-watchdog" \
  "$BUILD_DIR/aether-systemui" "$BUILD_DIR/aether-sysuictl" \
  "$BUILD_DIR/aether-hal-android" \
  "$BUILD_DIR/aether-settings" "$BUILD_DIR/aether-clock" \
  "$BUILD_DIR/aether-updater"

# ---------- 4. 宿主 IPC 契约冒烟 ----------
run_stage host-build gcc -O2 -Wall -DAETHEROS_BUILD='"ci"' -o "$BUILD_DIR/aetherosd-host" "$REPO_ROOT/aetherosd.c"
run_stage ipc-smoke bash "$REPO_ROOT/ci/ipc-contract-smoke.sh" \
  "$BUILD_DIR/aetherosd-host" "$BUILD_DIR/ipc-smoke"

# ---------- 5. busybox（缺失则下载并交叉编译；存在则增量） ----------
run_stage busybox bash "$REPO_ROOT/ci/build-busybox.sh" /tmp/linux-build

# ---------- 6. rootfs（参数化路径 -> BUILD_DIR） ----------
run_stage rootfs env \
  BB="${BB:-/tmp/linux-build/busybox-1.36.1/_install}" \
  ROOT="$BUILD_DIR/rootfs" \
  IMG="$BUILD_DIR/aetheros-rootfs.ext4" \
  AETHER_INIT="$BUILD_DIR/aether-init-aarch64" \
  AETHEROSD="$BUILD_DIR/aetherosd-aarch64-static" \
  AETHER_COMP="$BUILD_DIR/aether-compositor" \
  AETHER_DEMO="$BUILD_DIR/aether-demo" \
  AETHER_OTA="$BUILD_DIR/aether-ota-aarch64" \
  AETHER_PKG_HOST="$BUILD_DIR/aether-pkg-host" \
  AETHER_PKG="$BUILD_DIR/aether-pkg-aarch64" \
  AETHER_SANDBOX_RUN="$BUILD_DIR/aether-sandbox-run" \
  AETHER_LOGD="$BUILD_DIR/aether-logd" \
  AETHER_LOGCTL="$BUILD_DIR/aether-logctl" \
  AETHER_CRASHD="$BUILD_DIR/aether-crashd" \
  AETHER_WATCHDOG="$BUILD_DIR/aether-watchdog" \
  AETHER_SYSUI="$BUILD_DIR/aether-systemui" \
  AETHER_SYSUICTL="$BUILD_DIR/aether-sysuictl" \
  AETHER_HAL_ANDROID="$BUILD_DIR/aether-hal-android" \
  AETHER_SETTINGS="$BUILD_DIR/aether-settings" \
  AETHER_CLOCK="$BUILD_DIR/aether-clock" \
  AETHER_UPDATER="$BUILD_DIR/aether-updater" \
  AETHER_SIGN_KEY="$REPO_ROOT/ci/test-signing.key" \
  bash "$REPO_ROOT/make_rootfs.sh"

# ---------- 6b. P2-11: 运行时安装测试包（进 9p share，由 boot-smoke P 段安装） ----------
#   hello.aepkg        —— 发布密钥签名，CI 在客机内真实 install + 沙箱运行
#   hello-foreign.aepkg —— 外国密钥签名（现场生成），必须被拒（证明钉死的公钥
#                          不是摆设：格式合法、签名自洽，仅签名者不对）
run_stage pkg-test-assets bash -c "
  mkdir -p '$BUILD_DIR/share' &&
  AETHER_PKG_PRIVKEY='$REPO_ROOT/ci/test-signing.key' \
    '$BUILD_DIR/aether-pkg-host' create '$BUILD_DIR/share/hello.aepkg' \
    aether.hello 'AetherOS Hello' 1.0.0 hello '$BUILD_DIR/aether-hello=hello' &&
  '$BUILD_DIR/aether-pkg-host' keygen '$BUILD_DIR/foreign-priv.hex' '$BUILD_DIR/foreign-pub.hex' >/dev/null &&
  AETHER_PKG_PRIVKEY='$BUILD_DIR/foreign-priv.hex' \
    '$BUILD_DIR/aether-pkg-host' create '$BUILD_DIR/share/hello-foreign.aepkg' \
    evil.hello 'Evil Hello' 1.0.0 hello '$BUILD_DIR/aether-hello=hello' &&
  '$BUILD_DIR/aether-pkg-host' verify '$BUILD_DIR/share/hello.aepkg'"

# ---------- 7. QEMU 启动冒烟 ----------
if [ "$SKIP_BOOT" -eq 1 ]; then
  log "[boot-smoke] 跳过（--skip-boot）"
else
  mkdir -p "$BUILD_DIR/share"
  # 默认冒烟：附带真实 virtio 键盘（P1-2a，需内核 CONFIG_VIRTIO_INPUT）
  run_stage boot-smoke python3 "$REPO_ROOT/ci/boot-smoke.py" \
    --kernel "$REPO_ROOT/aetheros-kernel-6.6.70-aarch64.Image" \
    --rootfs "$BUILD_DIR/aetheros-rootfs.ext4" \
    --share "$BUILD_DIR/share" \
    --keyboard \
    --tablet \
    --log "$BUILD_DIR/boot-smoke.log" \
    --summary "$BUILD_DIR/boot-smoke.summary.txt"

  # 手机竖屏形态冒烟（720x1600）：这就是发布到手机的形态——VNC 铺满整块
  # 手机屏。SystemUI 走竖屏布局（状态条 96/行高 148/时钟 6x），J16/T2/U7
  # 的坐标全部由该模式推导（与横屏基线共用同一套断言，不是放宽）。
  run_stage boot-smoke-portrait python3 "$REPO_ROOT/ci/boot-smoke.py" \
    --kernel "$REPO_ROOT/aetheros-kernel-6.6.70-aarch64.Image" \
    --rootfs "$BUILD_DIR/aetheros-rootfs.ext4" \
    --share "$BUILD_DIR/share" \
    --keyboard \
    --tablet \
    --xres 720 --yres 1600 \
    --log "$BUILD_DIR/boot-smoke-portrait.log" \
    --summary "$BUILD_DIR/boot-smoke-portrait.summary.txt"

  # P1-2e: virgl GPU 3D 能力冒烟（宿主需 libvirglrenderer + Mesa，egl-headless）
  run_stage boot-smoke-virgl python3 "$REPO_ROOT/ci/boot-smoke.py" \
    --kernel "$REPO_ROOT/aetheros-kernel-6.6.70-aarch64.Image" \
    --rootfs "$BUILD_DIR/aetheros-rootfs.ext4" \
    --share "$BUILD_DIR/share" \
    --gpu-virgl \
    --tablet \
    --log "$BUILD_DIR/boot-smoke-virgl.log" \
    --summary "$BUILD_DIR/boot-smoke-virgl.summary.txt"

  # ---------- 7b. P1-2c: A/B 双槽 OTA 冒烟 ----------
  # 构建双槽镜像组（slotA/slotB/persist/initramfs 选择器）后跑 ab-smoke：
  # 拒绝坏槽 -> 切槽实启 -> 恢复模式 -> 优雅停机。
  run_stage ab-images env \
    OUT="$BUILD_DIR/ab" \
    BB="${BB:-/tmp/linux-build/busybox-1.36.1/_install}" \
    AETHER_INIT="$BUILD_DIR/aether-init-aarch64" \
    AETHEROSD="$BUILD_DIR/aetherosd-aarch64-static" \
    AETHER_COMP="$BUILD_DIR/aether-compositor" \
    AETHER_DEMO="$BUILD_DIR/aether-demo" \
    AETHER_PKG_HOST="$BUILD_DIR/aether-pkg-host" \
    AETHER_PKG="$BUILD_DIR/aether-pkg-aarch64" \
    AETHER_SANDBOX_RUN="$BUILD_DIR/aether-sandbox-run" \
  AETHER_LOGD="$BUILD_DIR/aether-logd" \
  AETHER_LOGCTL="$BUILD_DIR/aether-logctl" \
  AETHER_CRASHD="$BUILD_DIR/aether-crashd" \
  AETHER_WATCHDOG="$BUILD_DIR/aether-watchdog" \
  AETHER_SYSUI="$BUILD_DIR/aether-systemui" \
  AETHER_SYSUICTL="$BUILD_DIR/aether-sysuictl" \
  AETHER_HAL_ANDROID="$BUILD_DIR/aether-hal-android" \
  AETHER_SETTINGS="$BUILD_DIR/aether-settings" \
  AETHER_CLOCK="$BUILD_DIR/aether-clock" \
    AETHER_SIGN_KEY="$REPO_ROOT/ci/test-signing.key" \
    bash "$REPO_ROOT/ci/make_ab.sh"

  run_stage ab-smoke python3 "$REPO_ROOT/ci/ab-smoke.py" \
    --kernel "$REPO_ROOT/aetheros-kernel-6.6.70-aarch64.Image" \
    --initrd "$BUILD_DIR/ab/aetheros-ab-initramfs.cpio.gz" \
    --slot-a "$BUILD_DIR/ab/slotA.ext4" \
    --slot-b "$BUILD_DIR/ab/slotB.ext4" \
    --persist "$BUILD_DIR/ab/persist.ext4" \
    --log "$BUILD_DIR/ab-smoke.log" \
    --summary "$BUILD_DIR/ab-smoke.summary.txt"
fi

# ---------- 汇总 ----------
stage summary "本地 CI 模拟结果"
log "证据日志: $EVIDENCE"
log "构建目录: $BUILD_DIR"
if [ "${#FAILED_STAGES[@]}" -eq 0 ]; then
  log "LOCAL_CI_OK —— 全部阶段通过"
  exit 0
else
  log "LOCAL_CI_FAILED —— 失败阶段: ${FAILED_STAGES[*]}"
  exit 1
fi
