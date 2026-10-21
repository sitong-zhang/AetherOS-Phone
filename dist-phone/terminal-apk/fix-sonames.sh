#!/bin/bash
# 把带版本号的 .so 规范成 Android 能提取的 lib*.so 名字（幂等）。
#
# 背景（phone-v9 真机 QEMU 启动即 exit code=1 的根因）：
#   Android 的 installd 只会从 APK 的 lib/<abi>/ 里提取 **文件名匹配 lib*.so** 的条目。
#   Termux 打包的 QEMU 依赖 libz.so.1 / libglib-2.0.so.0 之类“以版本号结尾”的文件名，
#   它们在 APK 里存在、却永远不会被解到 nativeLibraryDir，于是 bionic linker 在 exec
#   qemu-system-aarch64 时报 CANNOT LINK EXECUTABLE 并 exit(1) —— 表现为“QEMU 已启动”
#   紧跟一行“QEMU 退出 code=1”。
#
# 做法：改 SONAME + 改文件名 + 重写所有 DT_NEEDED 引用。ELF 内部符号表不动。
set -euo pipefail
shopt -s nullglob

DIR="${1:-$(cd "$(dirname "$0")" && pwd)/lib/arm64-v8a}"
cd "$DIR"

# 版本化文件名 -> 合规文件名
MAP=(
  "libbz2.so.1.0:libbz2.so"
  "libgio-2.0.so.0:libgio-2.0.so"
  "libglib-2.0.so.0:libglib-2.0.so"
  "libgmodule-2.0.so.0:libgmodule-2.0.so"
  "libgobject-2.0.so.0:libgobject-2.0.so"
  "libhogweed.so.6:libhogweed.so"
  "libjpeg.so.8:libjpeg.so"
  "libncursesw.so.6:libncursesw.so"
  "libnettle.so.8:libnettle.so"
  "libz.so.1:libz.so"
  "libzstd.so.1:libzstd.so"
)

echo "[soname] 规范化版本化库名（否则 Android 不会提取，QEMU 必然 exit 1）"
for pair in "${MAP[@]}"; do
  old="${pair%%:*}"; new="${pair##*:}"
  if [ -f "$old" ]; then
    patchelf --set-soname "$new" "$old"
    mv -f "$old" "$new"
    echo "  $old -> $new"
  fi
done

echo "[soname] 重写 DT_NEEDED 引用"
for f in *.so *.so.*; do
  [ -f "$f" ] || continue
  for pair in "${MAP[@]}"; do
    old="${pair%%:*}"; new="${pair##*:}"
    if readelf -d "$f" 2>/dev/null | grep -q "\[$old\]"; then
      patchelf --replace-needed "$old" "$new" "$f"
    fi
  done
done

echo "[soname] 重写 RUNPATH -> \$ORIGIN"
# 这是 phone-v9/v10 在真机“QEMU 退出 code=1”的真正原因，比库名更致命：
#   Termux 的所有库把 RUNPATH 硬编码成 /data/data/com.termux/files/usr/lib。
#   这个目录在我们的 App 里不存在，而 Android 的 bionic linker 对普通应用
#   **会忽略 LD_LIBRARY_PATH**（只有 shell / debuggable 进程有豁免）。
#   于是 exec QEMU 时，DT_NEEDED 里的 libglib-2.0.so 等一个也解析不到，
#   linker 直接 CANNOT LINK EXECUTABLE 并 exit(1)。
# 改成 $ORIGIN 后，linker 从“库自己所在的目录”（即 nativeLibraryDir）解析，
# 整包自包含、不依赖任何外部路径。$ORIGIN 必须用单引号，别让 shell 展开。
for f in *.so; do
  [ -f "$f" ] || continue
  patchelf --set-rpath '$ORIGIN' "$f"
done

echo "[compat] 生成符号补齐 stub（libdwfl/pulse/jack/alsa/libusb/spice/usbredir/ssh/curl）"
# bionic 加载时会急切解析 QEMU 的全部 UND 符号；这些可选功能库不在包里，
# 但符号一个都不能缺（phone-v12 真机：cannot locate symbol "dwfl_standard_find_debuginfo"）
python3 "$(cd "$(dirname "$0")" && pwd)/make-compat-stub.py" "$DIR"

echo "[verneed] 清除孤儿版本需求"
# bionic 要求 verneed 里每个 File 都必须在 DT_NEEDED，否则直接拒绝加载：
#   cannot find "libdw.so.1" from verneed[5] in DT_NEEDED list
# Termux 的 QEMU 带着指向 libdw.so.1 / libpulse.so 的死引用（这两个库根本
# 不在包里，glibc 容忍，Android 不容忍）——phone-v11 真机故障的真凶。
python3 "$(cd "$(dirname "$0")" && pwd)/fix-verneed.py" "$DIR"

echo "[verneed] 校验 verneed 与 DT_NEEDED 一致"
badvn=0
for f in *.so; do
  [ -f "$f" ] || continue
  # 只校验动态段里仍有 VERNEED 的库（清过版本的孤儿段不算数）
  readelf -dW "$f" 2>/dev/null | grep -q VERNEED || continue
  vn="$(readelf -VW "$f" 2>/dev/null | grep ' File: ' | sed 's/.*File: //;s/ .*//' | sort -u)"
  nd="$(readelf -dW "$f" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | sort -u)"
  while IFS= read -r lib; do
    [ -z "$lib" ] && continue
    grep -qx "$lib" <<<"$nd" || { echo "  ORPHAN VERNEED: $lib (in $f，Android 会拒绝加载)" >&2; badvn=1; }
  done <<<"$vn"
done
[ "$badvn" = "0" ] || exit 1

echo "[soname] 校验依赖闭包（按文件名）"
missing=0
for f in *.so *.so.*; do
  [ -f "$f" ] || continue
  for n in $(readelf -d "$f" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
    case "$n" in
      libc.so|libm.so|libdl.so|liblog.so|libstdc++.so|libandroid.so) continue;;
    esac
    [ -f "$n" ] || { echo "  MISSING: $n (needed by $f)" >&2; missing=1; }
  done
done
[ "$missing" = "0" ] || { echo "FATAL: 依赖闭包不完整" >&2; exit 1; }

bad=0
for f in *.so *.so.*; do
  case "$f" in
    lib*.so) ;;
    *) echo "  BAD NAME: $f（不以 .so 结尾，Android 不会提取）" >&2; bad=1;;
  esac
done
[ "$bad" = "0" ] || exit 1

badrp=0
for f in *.so; do
  rp="$(patchelf --print-rpath "$f" 2>/dev/null || true)"
  case "$rp" in
    *'$ORIGIN'*) ;;
    *) echo "  BAD RUNPATH: $f -> '$rp'（Android 会忽略 LD_LIBRARY_PATH，必须 \$ORIGIN）" >&2; badrp=1;;
  esac
done
[ "$badrp" = "0" ] || exit 1
echo "[soname] OK：$(ls -1 *.so | wc -l) 个库全部可被 Android 提取，且 RUNPATH=\$ORIGIN"
