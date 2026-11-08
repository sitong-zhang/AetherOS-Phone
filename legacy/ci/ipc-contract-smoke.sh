#!/bin/bash
# ipc-contract-smoke.sh —— aetherosd IPC 契约宿主冒烟（不依赖 QEMU）
#
# 对一个「宿主机可执行」的 aetherosd 二进制验证 AETHEROSD-PROTOCOL.md 契约：
#   0) 前置：/run/aetheros.sock 必须不存在（保证测的是本次启动的守护进程）
#   1) 负例：无守护进程时 `aetherosd -c` 必须退出码 1 且明说 "cannot reach"
#   2) 启动守护进程，等待 socket 真实出现（bind+listen）
#   3) 访问控制：socket 文件 mode 必须是 0660（P0-3 内核准入）
#   4) JSON 契约：schema/backend/cpus/memory/root_volume 字段逐个断言
#   5) 活数据：两次调用 uptime_s 必须递增（不是缓存/假数据）
#   6) 单次模式 `-1`：状态文件被真实写出且含 schema
#   7) 清理：守护进程退出后 socket 必须被 unlink（协议承诺）
#
# 用法: ipc-contract-smoke.sh <host-executable-aetherosd> [workdir]
# 退出: 0 = 全部通过; 1 = 任一断言失败; 2 = 用法/前置错误
set -u

BIN="${1:-}"
WORK="${2:-/tmp/aether-ipc-smoke}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "usage: $0 <host-executable-aetherosd> [workdir]" >&2
  exit 2
fi
# 契约里的 SOCK_PATH/STATE_PATH 编译期为 /run/...，宿主运行需要 root；
# GitHub runner 默认非 root —— 用 sudo（runner 免密）。本机 root 则不需要。
SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi
SOCK=/run/aetheros.sock
STATE=/run/aetheros.state

mkdir -p "$WORK"
LOG="$WORK/daemon.log"
DPID=""

cleanup() {
  if [ -n "$DPID" ] && kill -0 "$DPID" 2>/dev/null; then
    kill "$DPID" 2>/dev/null
    wait "$DPID" 2>/dev/null
  fi
  $SUDO rm -f "$SOCK" "$STATE" 2>/dev/null
}
trap cleanup EXIT

pass() { echo "[PASS] $1"; }
FAILED=0
fail() { echo "[FAIL] $1"; FAILED=$((FAILED + 1)); }

# 宿主可执行性前置：aarch64 产物在本脚本里跑不了（Exec format error），
# 它由 boot-smoke.yml 在 QEMU 内覆盖——此处明确要求 x86-64。
if ! file -b "$BIN" | grep -qE 'x86-64|x86_64'; then
  echo "[FAIL] $BIN 不是宿主机可执行二进制（$(file -b "$BIN" | cut -c1-60)...）。" >&2
  echo "       aarch64 产物请使用 boot-smoke.yml（QEMU 内覆盖）。" >&2
  exit 2
fi

# 0) 前置：socket 不得已存在
if [ -e "$SOCK" ]; then
  echo "[FAIL] $SOCK 已存在——本次冒烟拒绝在来路不明的 socket 上测试" >&2
  exit 2
fi

echo "== aetherosd IPC 契约冒烟: $BIN =="

# 1) 负例：守护进程缺席时客户端必须诚实失败
neg_out=$($SUDO "$BIN" -c 2>&1); neg_rc=$?
if [ "$neg_rc" -eq 1 ]; then
  pass "负例: 无守护进程时 -c 退出码=1"
else
  fail "负例: 无守护进程时 -c 退出码=$neg_rc（期望 1）: $neg_out"
fi
if echo "$neg_out" | grep -q "cannot reach $SOCK"; then
  pass "负例: 错误信息明确 'cannot reach $SOCK'"
else
  fail "负例: 错误信息不含 'cannot reach $SOCK': $neg_out"
fi

# 2) 启动守护进程，等待真 socket
$SUDO "$BIN" >"$LOG" 2>&1 &
DPID=$!
sock_ok=0
for _ in $(seq 1 50); do
  if [ -S "$SOCK" ]; then sock_ok=1; break; fi
  if ! kill -0 "$DPID" 2>/dev/null; then break; fi
  sleep 0.2
done
if [ "$sock_ok" -eq 1 ]; then
  pass "守护进程启动并 bind $SOCK"
else
  fail "守护进程启动后 socket 未出现（日志: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')）"
  exit 1
fi

# 3) 访问控制：0660
mode=$($SUDO stat -c '%a' "$SOCK" 2>/dev/null)
if [ "$mode" = "660" ]; then
  pass "socket mode=0660（内核准入）"
else
  fail "socket mode=$mode（期望 660）"
fi

# 4) JSON 契约字段
snap=$($SUDO "$BIN" -c 2>/dev/null)
if [ -z "$snap" ]; then
  fail "-c 未取到快照"
  exit 1
fi
expect_substr() { # expect_substr <描述> <字面>
  if printf '%s' "$snap" | grep -qF "$2"; then pass "$1"; else fail "$1（缺失: $2）"; fi
}
expect_substr "schema = aetherosd.state/1" '"schema": "aetherosd.state/1"'
expect_substr "backend = linux-native"    '"backend": "linux-native"'
expect_substr "root_volume.mount = /"     '"mount": "/"'
if printf '%s' "$snap" | grep -qE '"cpus": [1-9]'; then
  pass "cpus >= 1（真实读数）"
else
  fail "cpus 不是 >=1 的真实读数"
fi
if printf '%s' "$snap" | grep -qE '"total_kb": [1-9]'; then
  pass "memory.total_kb > 0（真实读数）"
else
  fail "memory.total_kb 非正"
fi
if printf '%s' "$snap" | grep -qE '"uptime_s": [0-9]'; then
  pass "uptime_s 为非负实数"
else
  fail "uptime_s 异常（应为 >=0 或 -1，绝不编造）"
fi

# 5) 活数据：两次调用 uptime 递增
u1=$(printf '%s' "$snap" | sed -n 's/.*"uptime_s": \([0-9]*\).*/\1/p')
sleep 2
snap2=$($SUDO "$BIN" -c 2>/dev/null)
u2=$(printf '%s' "$snap2" | sed -n 's/.*"uptime_s": \([0-9]*\).*/\1/p')
if [ -n "$u1" ] && [ -n "$u2" ] && [ "$u2" -gt "$u1" ]; then
  pass "活数据: uptime_s 递增（$u1 -> $u2）"
else
  fail "活数据: uptime_s 未递增（$u1 -> $u2）"
fi

# 6) 单次模式 -1：状态文件真实写出
if $SUDO "$BIN" -1 >/dev/null 2>&1 && $SUDO grep -q '"schema": "aetherosd.state/1"' "$STATE" 2>/dev/null; then
  pass "单次模式 -1 写出 $STATE 且含 schema"
else
  fail "单次模式 -1 未写出合规状态文件"
fi

# 7) 清理：退出后 socket 被 unlink
kill "$DPID" 2>/dev/null
wait "$DPID" 2>/dev/null
DPID=""
gone=0
for _ in $(seq 1 25); do
  [ ! -e "$SOCK" ] && { gone=1; break; }
  sleep 0.2
done
if [ "$gone" -eq 1 ]; then
  pass "守护进程退出后 socket 已清理"
else
  fail "守护进程退出后 socket 仍存在"
fi

if [ "$FAILED" -eq 0 ]; then
  echo "IPC_CONTRACT_OK"
  exit 0
else
  echo "IPC_CONTRACT_FAILED ($FAILED 项断言失败)"
  exit 1
fi
