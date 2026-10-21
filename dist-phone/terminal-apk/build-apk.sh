#!/bin/bash
# AetherOS 自包含终端 APK 构建脚本（phone-v18）
# 架构：APK 内嵌 QEMU + Java 内嵌服务 + WebView 前端，无需 Termux。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE"
BUILD="$HERE/build"
OUT="$HERE/aetheros-terminal.apk"

ANDROID_SDK="${ANDROID_SDK:-/opt/android-sdk}"
# 必须用 35+：34 的 zipalign 没有 -P（16KB 页对齐），而 Android 15/16 需要它。
BT="$ANDROID_SDK/build-tools/35.0.0"
[ -x "$BT/zipalign" ] || BT="$ANDROID_SDK/build-tools/34.0.0"
AJAR="$ANDROID_SDK/platforms/android-34/android.jar"
KEYSTORE="${KEYSTORE:-$HOME/.android/debug.keystore}"
KS_PASS="${KS_PASS:-android}"
KS_ALIAS="${KS_ALIAS:-androiddebugkey}"

echo "[1/8] 清理构建目录"
rm -rf "$BUILD"; mkdir -p "$BUILD/obj" "$BUILD/dex" "$BUILD/compiled"

echo "[2/8] 规范化 .so 名字（Android 只提取 lib*.so）"
"$SRC/fix-sonames.sh" "$SRC/lib/arm64-v8a"

echo "[3/8] javac 编译 Java 源码"
javac -source 8 -target 8 \
  -bootclasspath "$AJAR" \
  -classpath "$AJAR" \
  -d "$BUILD/obj" \
  "$SRC"/src/com/aetheros/terminal/*.java

echo "[4/8] d8 生成 classes.dex"
"$BT/d8" --release --min-api 24 \
  --lib "$AJAR" \
  --output "$BUILD/dex" \
  $(find "$BUILD/obj" -name '*.class')

echo "[5/8] aapt2 编译资源"
"$BT/aapt2" compile --dir "$SRC/res" -o "$BUILD/compiled/res.zip"

echo "[6/8] aapt2 链接基础 APK（.so 不压缩）"
"$BT/aapt2" link -o "$BUILD/base.apk" \
  -I "$AJAR" \
  --manifest "$SRC/AndroidManifest.xml" \
  "$BUILD/compiled/res.zip" \
  -0 so

echo "[7/8] 组装：assets + dex + lib/arm64-v8a"
cp "$BUILD/base.apk" "$BUILD/app.apk"
# 关键：必须在 $SRC 下打包，让条目成为 assets/xxx。
# 若 cd 进 assets 再打包，条目会落在归档根目录，AssetManager 只认 assets/ 下的路径，
# 结果就是 getAssets().open("index.html") 必然失败 -> 前端拿不到 -> 纯黑屏。
(cd "$SRC" && zip -q -r "$BUILD/app.apk" assets)
(cd "$BUILD/dex" && zip -q "$BUILD/app.apk" classes.dex)
# lib 目录不压缩存储（extractNativeLibs=true 时安装器会提取；不压缩 + 页对齐可被直接 mmap）
# 注意：APK 内必须是 lib/arm64-v8a/ 路径
(cd "$SRC" && zip -q -0 -r "$BUILD/app.apk" lib/arm64-v8a)

echo "[8/8] zipalign(-P 16) + 签名"
cp "$BUILD/app.apk" "$BUILD/aligned.apk"
# 必须用 -P 16，不能用 -p（-p 只做 4KB 页对齐）。
# Android 15 起对 APK 内“未压缩的 .so”强制 16KB 页对齐，Android 16 更严；
# 4KB 对齐时 linker 在加载阶段就失败，QEMU 进程活不到一秒就退出，
# 表现就是“点运行闪一下、状态变未启动、VNC 5900 永远连不上”（phone-v10 的真机故障）。
# -P 与 -p 互斥，二者只能取一。
# 注意 pipefail：zipalign 无参数时自身退出码是 1，必须用子 shell 兜住，
# 否则 grep 命中了也会被判成“不支持 -P”。
if ( "$BT/zipalign" 2>&1 || true ) | grep -q -- "-P "; then
  "$BT/zipalign" -P 16 -f 4 "$BUILD/aligned.apk" "$OUT"
else
  echo "WARN: 该 zipalign 不支持 -P，退回 4KB 对齐（离 16KB 只差对齐，不影响解压式安装）" >&2
  "$BT/zipalign" -p -f 4 "$BUILD/aligned.apk" "$OUT"
fi
rm -f "$OUT.idsig"
"$BT/apksigner" sign \
  --ks "$KEYSTORE" --ks-pass pass:"$KS_PASS" \
  --key-pass pass:"$KS_PASS" --ks-key-alias "$KS_ALIAS" \
  --min-sdk-version 24 --max-sdk-version 34 \
  "$OUT"

echo ""
echo "=== 构建完成 ==="
ls -la "$OUT"

# ---- 硬性自检：这两个错误历史上各造成一次“装完打不开” ----
fail=0
LIST="$(unzip -l "$OUT")"
# 用 grep -c（读完全部输入，不会因提前退出让上游吃 SIGPIPE 触发 pipefail 误判）
has() { [ "$(printf '%s\n' "$LIST" | grep -c "$1\$")" -gt 0 ]; }
has "assets/index.html" || { echo "FATAL: APK 内缺少 assets/index.html（资源打包路径错误，会导致黑屏）" >&2; fail=1; }
has "assets/app.js"     || { echo "FATAL: APK 内缺少 assets/app.js" >&2; fail=1; }
has "assets/favicon.ico" || { echo "FATAL: APK 内缺少 assets/favicon.ico（WebView 每次加载都请求，缺了会刷 404 噪音）" >&2; fail=1; }
has "lib/arm64-v8a/libqemu-system-aarch64.so" \
  || { echo "FATAL: APK 内缺少 QEMU 引擎 .so" >&2; fail=1; }
has "assets/qemu-data/keymaps/en-us" \
  || { echo "FATAL: APK 内缺少 keymaps（未指 -L 会没有键盘）" >&2; fail=1; }
# Android installd 只提取 lib*.so；带版本后缀的库永远不会进 nativeLibraryDir，
# 结果是 exec QEMU 时 CANNOT LINK EXECUTABLE -> exit 1（phone-v9 的真机故障）。
BADLIB="$(unzip -l "$OUT" | awk '{print $4}' | grep '^lib/arm64-v8a/' | grep -v '/$' | grep -v '\.so$' || true)"
if [ -n "$BADLIB" ]; then
  echo "FATAL: 这些库名 Android 不会提取（必须以 .so 结尾）:" >&2
  echo "$BADLIB" >&2
  fail=1
fi
SDKLINE=$("$BT/aapt2" dump badging "$OUT" 2>/dev/null | grep -E "^sdkVersion|^targetSdkVersion" | tr '\n' ' ')
echo "SDK: $SDKLINE"
echo "$SDKLINE" | grep -q "targetSdkVersion:'34'" \
  || { echo "FATAL: targetSdkVersion 不是 34，系统会弹“旧版本”警告" >&2; fail=1; }
# 16KB 页对齐自检：用 zipalign 自己的 -c（权威），不要手算偏移
if ( "$BT/zipalign" 2>&1 || true ) | grep -q -- "-P "; then
  if ! "$BT/zipalign" -c -P 16 4 "$OUT" >/dev/null 2>&1; then
    echo "FATAL: APK 内 .so 未按 16KB 页对齐（Android 15+ 可能加载失败）" >&2
    "$BT/zipalign" -c -P 16 -v 4 "$OUT" 2>&1 | grep -v "(OK" | head -6 >&2
    fail=1
  fi
fi
[ "$fail" = "0" ] || exit 1

"$BT/aapt2" dump badging "$OUT" 2>/dev/null | grep -E "package|application-label|launchable" || true
echo "APK 内 .so 数量: $(unzip -l "$OUT" | grep -c 'lib/arm64-v8a/.*\.so')"
unzip -l "$OUT" | grep 'lib/arm64-v8a/' | head -5