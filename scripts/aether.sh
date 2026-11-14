#!/usr/bin/env bash
# aether.sh - AetherOS 六种一键操作（v2 §2.2）
#
# 所属层：L7 生态与工程 / 构建与运维入口
# 依赖：bash、gcc、python3（环境基线由 tools/collect_baseline.py 采集）
# 对外接口：aether.sh {build|start|stop|rebuild|test|evidence|status} [--track A|B]
# 不变量：
#   - 无机器绝对路径：所有路径由脚本自身位置推导，或用 AETHER_OUT 覆盖（§2.4 / §10.3）
#   - 缺依赖报「缺什么」而不是笼统「失败」；未就绪的能力明确标注，不假装成功
#   - 构建产物落仓库内 build/，由 .gitignore 排除；不写 /usr、/run 等系统目录
#   - 关闭后不留残留进程、挂载、锁文件
# 对应验收项：§2.2 六种操作、§2.5 可复现性、§6.2 Gate S1、§15.1 环境与工程
set -u
set -o pipefail   # 管道中任一命令失败即判失败，禁止 tee 吞掉机检/自测的非零退出码

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${AETHER_OUT:-$ROOT/build}"
EVID="${AETHER_EVIDENCE:-$ROOT/../aetheros-archive/evidence}"
# 顶层解析轨道参数（函数内取不到脚本位置参数，必须在此处算好）
TRACK=""
if [ "${2:-}" = "--track" ]; then TRACK="${3:-}"; fi
CC="${CC:-gcc}"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -I$ROOT/l2/include"

log()  { printf '[aether] %s\n' "$*"; }
die()  { printf '[aether][ERROR] %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "缺依赖：$1"; }

# ---------- 依赖检查 ----------
check_deps() {
    need gcc
    need python3
    need sha256sum
}

# ---------- L2 编译（真实可复现） ----------
compile_l2() {
    mkdir -p "$OUT/l2" "$OUT/gate"
    log "编译 L2 接口层 -> $OUT/l2"
    $CC $CFLAGS -c "$ROOT/l2/sim/aether_hal_core.c"   -o "$OUT/l2/aether_hal_core.o"   || die "aether_hal_core.c 编译失败"
    $CC $CFLAGS -c "$ROOT/l2/sim/aether_impl_sim.c"   -o "$OUT/l2/aether_impl_sim.o"   || die "aether_impl_sim.c 编译失败"
    $CC $CFLAGS -c "$ROOT/l2/sim/aether_impl_stub.c"  -o "$OUT/l2/aether_impl_stub.o"  || die "aether_impl_stub.c 编译失败"
    $CC $CFLAGS "$ROOT/l2/tests/test_hal.c" "$OUT"/l2/*.o -o "$OUT/l2/test_hal"        || die "test_hal 链接失败"
    log "L2 编译完成"
}

# ---------- 前置资产检查（A 轨整机，如实报缺什么） ----------
a_track_prereqs() {
    local missing=0
    local img="$ROOT/build/aarch64/Image"
    local fs="$ROOT/build/aarch64/rootfs.ext4"
    [ -f "$img" ] || { log "缺：内核镜像 $img（执行 bash boot/build.sh 从源码构建）"; missing=1; }
    [ -f "$fs" ]  || { log "缺：根文件系统 $fs（执行 bash boot/build.sh 自建）"; missing=1; }
    return $missing
}

cmd_build() {
    check_deps
    compile_l2
    # A 轨整机链路：如实检查前置，不假装能起
    log "检查 A 轨整机前置资产…"
    if a_track_prereqs; then
        log "A 轨前置资产齐备（启动：bash scripts/aether.sh start --track A）"
    else
        log "A 轨整机当前【未就绪】：执行 bash boot/build.sh 从源码构建内核与根文件系统"
    fi
    log "B 轨上层（x86 原生）已可构建；交互/像素验收待上层接入 L2 仿真实现后开展"
}

cmd_start() {
    check_deps
    # TRACK 已由顶层解析为全局值；函数内 $1/$2/$3 是函数自身位置参数（此处为空），
    # 不能在这里取脚本参数，否则 --track B 会被误判成默认 A 轨。
    case "${TRACK:-A}" in
        A|a)
            log "A 轨整机启动前置检查…"
            if ! a_track_prereqs; then
                die "A 轨未就绪：缺少内核镜像或根文件系统，需先执行 bash boot/build.sh"
            fi
            exec "$ROOT/boot/run-qemu.sh"
            ;;
        B|b)
            [ -x "$OUT/l2/test_hal" ] || compile_l2
            log "B 轨接口层 headless 冒烟（非整机界面，如实标注）"
            "$OUT/l2/test_hal"
            ;;
        *) die "未知轨道 ${TRACK}，可选 A（整机链路）/ B（原生交互）" ;;
    esac
}

cmd_stop() {
    log "检查残留进程 / 锁文件 / 挂载…"
    local pidf="$OUT/aether.pid" leftover=0
    if [ -f "$pidf" ]; then
        local pid; pid="$(cat "$pidf")"
        if kill -0 "$pid" 2>/dev/null; then
            log "终止进程 $pid"; kill "$pid" 2>/dev/null || true
        fi
        rm -f "$pidf"
    fi
    # 不主动 umount 任意路径（避免误伤宿主）；只检查并报告
    if mount 2>/dev/null | grep -q "$OUT"; then
        log "[WARN] $OUT 下存在挂载点，需人工确认后卸载"; leftover=1
    fi
    [ -f "$OUT/aether.lock" ] && { rm -f "$OUT/aether.lock"; log "已清理锁文件"; }
    if [ "$leftover" -eq 0 ]; then log "无残留进程 / 锁文件 / 挂载"; fi
}

cmd_rebuild() {
    check_deps
    log "清干净重来：删除 $OUT"
    rm -rf "$OUT"
    compile_l2
    # 可复现性：记录产物哈希，供两次构建对比（§2.5，时间戳除外）
    local hashfile="$OUT/build-hashes.txt"
    find "$OUT" -type f \( -name '*.o' -o -name 'test_hal' \) -print0 \
        | sort -z | xargs -0 sha256sum > "$hashfile" 2>/dev/null || true
    log "产物哈希 -> $hashfile"
    cat "$hashfile"
}

cmd_test() {
    check_deps
    [ -x "$OUT/l2/test_hal" ] || compile_l2
    mkdir -p "$EVID/round-001"
    local rc=0

    log "=== 1) L2 接口层自测（§5.4）==="
    "$OUT/l2/test_hal" 2>"$EVID/round-001/test_hal.stderr.txt" \
        | tee "$EVID/round-001/test_hal.stdout.txt" || rc=1

    log "=== 2) 五门机检（§5.6）==="
    python3 "$ROOT/tools/gate_l2.py" --out "$EVID/round-001/gate_l2.json" \
        | tee "$EVID/round-001/gate_l2.stdout.txt" || rc=1
    # 机检有 FAIL 时退出码非 0，这是如实结果，不掩盖
    log "=== 3) 环境基线（§2.1）==="
    python3 "$ROOT/tools/collect_baseline.py" \
        > "$EVID/round-001/baseline.stdout.txt" 2>&1 || rc=1

    log "测试完成 rc=$rc（非零表示存在未通过项，如实保留）"
    return $rc
}

cmd_evidence() {
    check_deps
    mkdir -p "$EVID/round-001"
    log "导出四件套证据（§2.7 / §11.4）-> $EVID/round-001"
    # 1 用例结果
    [ -f "$EVID/round-001/test_hal.stdout.txt" ] || \
        { [ -x "$OUT/l2/test_hal" ] || compile_l2; \
          "$OUT/l2/test_hal" > "$EVID/round-001/test_hal.stdout.txt" 2>"$EVID/round-001/test_hal.stderr.txt" || true; }
    # 2 异常报告（机检 FAIL 明细）
    python3 "$ROOT/tools/gate_l2.py" --out "$EVID/round-001/gate_l2.json" \
        > "$EVID/round-001/gate_l2.stdout.txt" 2>&1 || true
    # 3 改动 diff（git 跟踪范围内）
    git -C "$ROOT" status --short > "$EVID/round-001/changes.status.txt" 2>&1 || true
    git -C "$ROOT" diff --stat     > "$EVID/round-001/changes.diffstat.txt" 2>&1 || true
    # 4 回归前后对比由本地归档证据承载
    log "证据文件："
    ls -1 "$EVID/round-001"
}

cmd_status() {
    log "仓库：$ROOT"
    log "产物目录：$OUT"
    log "证据目录：$EVID"
    log "当前实现：$( [ -x "$OUT/l2/test_hal" ] && echo 'L2 已编译' || echo 'L2 未编译' )"
    if a_track_prereqs; then
        log "A 轨整机：内核镜像与根文件系统齐备，可启动"
    else
        log "A 轨整机：未就绪（执行 bash boot/build.sh 构建）"
    fi
    log "B 轨上层：接口层可跑；整机界面待接入 L2 仿真实现（S2）"
    log "第 1 门（无设备路径上浮）：FAIL（上层存量设备直连，详见机检输出）"
}

case "${1:-}" in
    build)    cmd_build ;;
    start)    cmd_start ;;
    stop)     cmd_stop ;;
    rebuild)  cmd_rebuild ;;
    test)     cmd_test ;;
    evidence) cmd_evidence ;;
    status)   cmd_status ;;
    secure)   shift; exec "$ROOT/scripts/secure.sh" "$@" ;;
    *)
        cat <<EOF
AetherOS 六种一键操作

用法: scripts/aether.sh <命令> [--track A|B]

  build     构建 L2 接口层 + 自测，并核对 A 轨前置资产
  start     拉起整机/原生上层：--track A（QEMU ARM64 整机链路）/ --track B（接口层 headless 冒烟）
  stop      正常关闭，清理进程/锁文件，检查挂载残留
  rebuild   清干净重来，产出可复现哈希
  test      全套自测 + 五门机检 + 基线采集，输出通过/失败清单
  evidence  导出四件套（用例结果/异常报告/改动 diff/回归对比）
  status    当前状态（如实标注已实现/部分实现/未开始）
  secure    安全整机入口：build/test/demo/evidence/status/shell/all（转交 scripts/secure.sh）

A 轨整机链路独立入口：
  bash boot/build.sh          从源码构建 Linux 6.6.70 ARM64 内核 + aarch64 用户态 + ext4 根盘
  bash boot/run-qemu.sh       QEMU TCG 冷启动并输出串口取证
  python3 tools/gate_boot.py  开机门禁机检

环境变量：AETHER_OUT、AETHER_EVIDENCE、AETHER_BUILD、CC、JOBS、BOOT_TIMEOUT
EOF
        [ -n "${1:-}" ] && die "未知命令：$1" || exit 0
        ;;
esac
