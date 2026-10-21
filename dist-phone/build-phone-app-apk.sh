#!/bin/sh
# build-phone-app-apk.sh —— 从 app/ 前端 + src/ Activity 源码重建「AetherOS App」APK。
#
# 依赖：JDK + Android SDK（build-tools 34.0.0 + platform android-34）
#   export ANDROID_SDK_ROOT=/opt/android-sdk
# 产出：aetheros-phone-app.apk（debug 签名，可直接安装）
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BT="${ANDROID_SDK_ROOT:-/opt/android-sdk}/build-tools/34.0.0"
AJ="${ANDROID_SDK_ROOT:-/opt/android-sdk}/platforms/android-34/android.jar"
BUILD="$HERE/.apk-build"
OUT="$HERE/aetheros-phone-app.apk"

rm -rf "$BUILD"; mkdir -p "$BUILD/obj" "$BUILD/compiled" "$BUILD/res/mipmap-nodpi" "$BUILD/assets" "$BUILD/src/com/aetheros/phoneapp"

# ---- 源码与资源就位（dist-phone/apk-src/ 为源，缺省用内联模板）----
SRC="$HERE/apk-src"
if [ -d "$SRC" ]; then
    cp "$SRC/AndroidManifest.xml" "$BUILD/"
    mkdir -p "$BUILD/res/values"
    cp "$SRC/res/values/styles.xml" "$BUILD/res/values/"
    cp -r "$SRC/src/." "$BUILD/src/"
    cp -r "$SRC/assets/." "$BUILD/assets/"
    cp "$SRC/ic_launcher.png" "$BUILD/res/mipmap-nodpi/" 2>/dev/null || true
fi
# 图标缺省取 app 前端图标
[ -f "$BUILD/res/mipmap-nodpi/ic_launcher.png" ] || cp "$HERE/app/icon-192.png" "$BUILD/res/mipmap-nodpi/ic_launcher.png"

echo "[build] javac ..."
javac -source 1.8 -target 1.8 -bootclasspath "$AJ" -classpath "$AJ" -d "$BUILD/obj" \
    "$BUILD/src/com/aetheros/phoneapp/MainActivity.java"

echo "[build] d8 ..."
"$BT/d8" --release --min-api 24 --output "$BUILD" "$BUILD"/obj/com/aetheros/phoneapp/*.class

echo "[build] aapt2 compile/link ..."
"$BT/aapt2" compile --dir "$BUILD/res" -o "$BUILD/compiled/res.zip"
"$BT/aapt2" link -o "$BUILD/app-unsigned.apk" -I "$AJ" \
    --manifest "$BUILD/AndroidManifest.xml" \
    --min-sdk-version 24 --target-sdk-version 34 \
    --version-code 1 --version-name 1.0.0 \
    -A "$BUILD/assets" "$BUILD/compiled/res.zip"

echo "[build] 追加 dex + 对齐 + 签名 ..."
( cd "$BUILD" && zip -q app-unsigned.apk classes.dex )
"$BT/zipalign" -f 4 "$BUILD/app-unsigned.apk" "$OUT"
KS="${AETHER_KEYSTORE:-$HOME/.android/debug.keystore}"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey "$OUT" 2>/dev/null || \
"$BT/apksigner" sign --ks "$KS" "$OUT"

"$BT/apksigner" verify "$OUT"
echo "[build] 完成: $OUT ($(du -h "$OUT" | cut -f1))"
