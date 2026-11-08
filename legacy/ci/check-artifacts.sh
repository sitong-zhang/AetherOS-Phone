#!/bin/bash
# check-artifacts.sh —— 交叉编译产物断言（CI 红绿灯的第一道闸）
#
# 断言每个传入文件都是「ARM aarch64 + 静态链接 + ELF 64-bit」。
# 动机：AetherOS 的 guest 用户态必须是自包含静态 ELF——guest rootfs 不带
# 交叉编译器 sysroot，动态链接二进制在 guest 里必然起不来。若产物不满足，
# 宁可 CI 红，也不能让一个"看起来编译过了"的假工件进入 rootfs。
#
# 用法: check-artifacts.sh <elf> [<elf> ...]
# 退出: 0 = 全部通过; 1 = 任一不满足（附具体原因）
set -u

fail=0
checked=0

check_one() {
  local f="$1" info
  if [ ! -f "$f" ]; then
    echo "[FAIL] $f: 文件不存在"
    return 1
  fi
  info=$(file -b "$f" 2>/dev/null)

  case "$info" in
    *"ELF 64-bit"*) ;;
    *) echo "[FAIL] $f: 不是 ELF 64-bit —— $info"; return 1 ;;
  esac
  case "$info" in
    *"ARM aarch64"*) ;;
    *) echo "[FAIL] $f: 不是 ARM aarch64 —— $info"; return 1 ;;
  esac
  case "$info" in
    *"statically linked"*) ;;
    *) echo "[FAIL] $f: 不是静态链接 —— $info"; return 1 ;;
  esac
  # 双重验证：readelf 看不到 dynamic section 才是真静态
  # （file 的 "statically linked" 读自 ELF 头，此处用段级证据交叉确认）
  if ! readelf -d "$f" 2>&1 | grep -q "There is no dynamic section"; then
    echo "[FAIL] $f: readelf 仍报告 dynamic section —— $info"
    return 1
  fi
  # 交叉确认机器类型（file 的文本匹配之外，直接读 ELF e_machine）
  local machine
  machine=$(readelf -h "$f" 2>/dev/null | sed -n 's/^  Machine: *//p')
  if [ "$machine" != "AArch64" ]; then
    echo "[FAIL] $f: ELF e_machine='$machine'（期望 AArch64）"
    return 1
  fi
  echo "[PASS] $f: ARM aarch64 静态 ELF（无 dynamic section）"
  return 0
}

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <elf> [<elf> ...]" >&2
  exit 2
fi

for f in "$@"; do
  checked=$((checked + 1))
  check_one "$f" || fail=1
done

if [ "$fail" -eq 0 ]; then
  echo "ARTIFACTS_OK ($checked 个工件全部通过)"
else
  echo "ARTIFACTS_FAILED ($checked 个受检, 存在不合格工件)"
fi
exit "$fail"
