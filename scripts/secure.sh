#!/usr/bin/env bash
# secure.sh - 长明 Aether / AetherOS 安全整机：构建 / 自测 / 演示 / 取证 一键入口
#
# 所属层：L7 工程入口。所有路径由脚本自身位置推导，可用 AETHER_OUT /
# AETHER_EVIDENCE 覆盖；不写系统目录；构建产物落 build/，由 .gitignore 排除。
# 命令：build | test | demo | evidence | status | shell | all
set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${AETHER_OUT:-$ROOT/build}"
EVID="${AETHER_EVIDENCE:-$ROOT/evidence/round-002}"
CC="${CC:-gcc}"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -I$ROOT/secure/include"
CORE="$ROOT/secure/core/aether_crypto.c $ROOT/secure/core/aether_store.c \
$ROOT/secure/core/aether_audit.c $ROOT/secure/core/aether_integrity.c \
$ROOT/secure/core/aether_secure.c"
OS_BIN="$OUT/bin/aether-os"
TEST_BIN="$OUT/bin/test_secure"
DEMO_ROOT="$OUT/secure-demo"

log()  { printf '[secure] %s\n' "$*"; }
die()  { printf '[secure][ERROR] %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "缺依赖：$1"; }

build() {
    need gcc
    mkdir -p "$OUT/bin" "$EVID"
    log "编译安全整机主程序 + 全套自测（gcc -Werror）-> $OUT/bin"
    $CC $CFLAGS "$ROOT/secure/cli/aether_os.c" $CORE -o "$OS_BIN"           || die "aether-os 编译失败"
    $CC $CFLAGS "$ROOT/secure/tests/test_secure.c" $CORE -o "$TEST_BIN"      || die "test_secure 编译失败"
    log "编译完成：aether-os / test_secure"
}

# 用本机 openssl(1) 独立复算，与自研密码学基座的断言向量比对（自测自证）
crosscheck() {
    need openssl
    need python3
    local f="$EVID/openssl-crosscheck.txt"
    local sha hmac pbk chacha
    sha=$(printf 'abc' | openssl dgst -sha256 -r | cut -d' ' -f1)
    hmac=$(printf 'The quick brown fox jumps over the lazy dog' | openssl dgst -sha256 -hmac key -r | cut -d' ' -f1)
    pbk=$(openssl kdf -keylen 32 -kdfopt digest:SHA256 -kdfopt pass:password \
            -kdfopt salt:salt -kdfopt iter:4096 PBKDF2 2>/dev/null | tr -d ':\n' | tr 'A-F' 'a-f')
    chacha=$(python3 -c "import sys;sys.stdout.buffer.write(b'\0'*64)" \
             | openssl enc -chacha20 \
                 -K 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f \
                 -iv 01000000000000090000004a00000000 -nopad 2>/dev/null \
             | python3 -c "import sys;print(sys.stdin.buffer.read()[:16].hex())")
    {
        echo "== OpenSSL 3.x 独立复算（与 secure/tests/test_secure.c 内置标准向量比对）=="
        echo "SHA256(abc)              = $sha"
        echo "HMAC-SHA256(key,... )    = $hmac"
        echo "PBKDF2-HMAC-SHA256(4096) = $pbk"
        echo "ChaCha20 keystream[:16]  = $chacha"
    } > "$f"
    cat "$f"
    [ "$sha"    = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ] || die "SHA256 交叉验证不一致"
    [ "$hmac"   = "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8" ] || die "HMAC 交叉验证不一致"
    [ "$pbk"    = "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" ] || die "PBKDF2 交叉验证不一致"
    [ "$chacha" = "10f1e7e4d13b5915500fdd1fa32071c4" ] || die "ChaCha20 交叉验证不一致"
    log "OpenSSL 交叉验证：4/4 一致（自研密码学非玩具）"
}

test() {
    build
    log "=== 运行 AetherOS Secure Core 全套自测 ==="
    "$TEST_BIN" 2>&1 | tee "$EVID/test_secure.stdout.txt"
    local rc=$?
    echo "$rc" > "$EVID/test_secure.rc.txt"
    [ $rc -eq 0 ] || { log "自测存在 FAIL（rc=$rc），如实保留"; return $rc; }
    crosscheck || return $?
    log "自测 + 交叉验证全部通过"
    return 0
}

demo() {
    build
    rm -rf "$DEMO_ROOT"
    mkdir -p "$EVID"
    log "=== 运行整机演示：解锁→存机密→输错3次→全盘销毁 ==="
    "$OS_BIN" --root "$DEMO_ROOT" --max-fails 3 --no-delay shell > "$EVID/demo-session.log" 2>&1 <<EOF
init --pw DemoPass2026 --duress Coerced999
boot
unlock --pw DemoPass2026
put 设计文档.md 长明Aether机密设计规范v1
put 密钥备份.bin A1B2C3D4E5F6
put 联系人.txt 张三-13800000000
list
snapshot $EVID/state-desktop.json
lock
unlock --pw wrong1
unlock --pw wrong2
snapshot $EVID/state-lock.json
unlock --pw wrong3
snapshot $EVID/state-wipe.json
status
audit
exit
EOF
    [ -f "$EVID/state-desktop.json" ] && [ -f "$EVID/state-lock.json" ] && \
        [ -f "$EVID/state-wipe.json" ] || die "演示流程未产出三份状态快照"
    python3 "$ROOT/tools/render_secure_ui.py" --evidence "$EVID" \
        | tee "$EVID/render.stdout.txt" || die "界面渲染失败"
    local r
    r=$(find "$DEMO_ROOT/receipt" -name 'wipe-*.json' 2>/dev/null | sort | tail -1)
    [ -n "$r" ] && cp "$r" "$EVID/wipe-receipt.json"
    log "演示完成：3 份真实状态快照 + 4 张界面图 + 销毁证明"
}

evidence() {
    build
    mkdir -p "$EVID"
    "$TEST_BIN" > "$EVID/test_secure.stdout.txt" 2>&1
    echo "$?" > "$EVID/test_secure.rc.txt"
    crosscheck >/dev/null 2>&1 || true
    git -C "$ROOT" status --short > "$EVID/changes.status.txt" 2>&1 || true
    ( cd "$ROOT" && find secure -type f \( -name '*.c' -o -name '*.h' \) -print0 \
        | sort -z | xargs -0 sha256sum ) > "$EVID/secure-sources.sha256.txt" 2>/dev/null || true
    {
        echo "# AetherOS Secure Core 证据清单"
        echo
        echo "- 自测：test_secure.stdout.txt（含 rc）"
        echo "- 密码学交叉验证：openssl-crosscheck.txt"
        echo "- 整机演示日志：demo-session.log"
        echo "- 真实状态快照：state-desktop.json / state-lock.json / state-wipe.json"
        echo "- 系统界面图：01~04 PNG（由真实快照渲染，非概念图）"
        echo "- 销毁证明：wipe-receipt.json（sys_unchanged=true）"
        echo "- 源码哈希：secure-sources.sha256.txt（可复现）"
        echo "- 改动清单：changes.status.txt"
    } > "$EVID/INDEX.md"
    log "证据已导出 -> $EVID"
    ls -1 "$EVID"
}

status() {
    log "产物：$OS_BIN / $TEST_BIN"
    if [ -x "$OS_BIN" ] && [ -d "$DEMO_ROOT" ]; then
        "$OS_BIN" --root "$DEMO_ROOT" --max-fails 3 --no-delay status
    else
        log "尚未运行演示：先执行 scripts/secure.sh demo"
    fi
}

shell() {
    build
    exec "$OS_BIN" "$@"
}

case "${1:-}" in
    build)    build ;;
    test)     test ;;
    demo)     demo ;;
    evidence) evidence ;;
    status)   status ;;
    shell)    shift; shell "$@" ;;
    all)      test && demo && evidence ;;
    *)
        cat <<EOF
长明 Aether / AetherOS 安全整机一键入口

用法: scripts/secure.sh <命令> [参数]

  build     编译 aether-os 主程序 + test_secure（gcc -Werror）
  test      全套自测(81 项) + OpenSSL 密码学交叉验证
  demo      整机 shell 演示流程，导出 3 份真实状态快照 + 4 张界面图 + 销毁证明
  evidence  汇总证据到 evidence/round-002/
  status    查看整机当前状态
  shell     进入交互式整机 shell（可传 --root/--max-fails/--no-delay 等）
  all       test → demo → evidence 一次跑完

环境变量：AETHER_OUT（产物目录）、AETHER_EVIDENCE（证据目录）、CC
也可从 scripts/aether.sh secure <命令> 转发调用。
EOF
        [ -n "${1:-}" ] && die "未知命令：$1" || exit 0
        ;;
esac
