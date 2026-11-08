#!/usr/bin/env bash
# Aether VM —— 全新独立 APK 构建脚本
# 从零编译：自研 Java / 自研 JNI(core) / SDL2 官方库层(照搬) / 内置内核与系统盘。
# 不使用任何第三方成品 APK 的 dex、资源或代码组织。
set -e
R=/workspace/aether-vm
A=$R/android
OUT=$R/out/apk
WORK=$R/build/work
BT=/opt/android-sdk/build-tools/34.0.0
AJAR=/opt/android-sdk/platforms/android-34/android.jar
NDK=/tmp/android-ndk-r25c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang
GLUE_SRC=/root/aether-archive/limbo-decompiled/smali/org/libsdl/app
ENGINE=/workspace/aether-aarch64-engine/libqemu-system-aarch64.so
KERNEL=/root/aether-archive/aether-linux/dist-phone/aetheros-kernel-aarch64.Image
DISK=/root/aether-archive/aether-aarch64-build/aetheros-system-v6.qcow2
LOGO=/workspace/aether-aarch64-engine/brand/AetherOS-logo-1024.png
KS=/root/aether-archive/aether-apk/aetheros.keystore
KSPW=$(cat /tmp/ks_pw.txt 2>/dev/null || echo aetheros123)

rm -rf "$WORK" "$OUT"; mkdir -p "$WORK" "$OUT" \
  "$WORK/classes" "$WORK/dex" "$WORK/glue" "$WORK/gen" "$WORK/res-compiled" \
  "$OUT/assets" "$OUT/lib/arm64-v8a" "$OUT/res/mipmap-hdpi" "$OUT/res/mipmap-xhdpi" "$OUT/res/mipmap-xxhdpi"

echo "=== [1/9] 生成 launcher 图标（官方 Logo，唯一允许自建资产）==="
python3 - <<PY
from PIL import Image
import os
src = Image.open("$LOGO").convert("RGBA")
for name, size in [("hdpi",72),("xhdpi",96),("xxhdpi",144)]:
    src.resize((size,size), Image.LANCZOS).save(
        "$OUT/res/mipmap-%s/ic_launcher.png" % name)
print("  已生成 3 档 launcher 图标")
PY
cp -r "$A/res/values" "$OUT/res/"

echo "=== [2/9] 编译资源（aapt2）==="
$BT/aapt2 compile --dir "$OUT/res" -o "$WORK/res-compiled/res.zip"
$BT/aapt2 link -o "$WORK/base.apk" -I "$AJAR" \
  --manifest "$A/AndroidManifest.xml" \
  -R "$WORK/res-compiled/res.zip" --auto-add-overlay \
  --java "$WORK/gen" --min-sdk-version 21 --target-sdk-version 29

echo "=== [3/9] 编译自研 Java（stub 仅供编译期解析 SDLActivity 父类）==="
mkdir -p "$WORK/stub"
javac -source 8 -target 8 -bootclasspath "$AJAR" -d "$WORK/stub" \
  "$A/stub/org/libsdl/app/SDLActivity.java" \
  "$A/stub/org/libsdl/app/SDLSurface.java"
javac -source 8 -target 8 -bootclasspath "$AJAR" -cp "$WORK/stub" \
  -d "$WORK/classes" $(find "$A/java" -name '*.java')
echo "  Java 类:"; find "$WORK/classes" -name '*.class' | sed 's|.*/classes/||'

echo "=== [4/9] d8 生成 classes.dex（业务层）==="
$BT/d8 --lib "$AJAR" --min-api 21 --output "$WORK/dex" \
  $(find "$WORK/classes" -name '*.class')
cp "$WORK/dex/classes.dex" "$OUT/classes.dex"

echo "=== [5/9] 汇编 SDL2 官方库层 org.libsdl.app.* → classes2.dex ==="
mkdir -p "$WORK/glue/org/libsdl"
cp -r "$GLUE_SRC" "$WORK/glue/org/libsdl/"
# 修正启动必崩点：随 Limbo 反编译出来的 SDLActivity 继承
# androidx.appcompat.app.AppCompatActivity，而独立工程不打包 androidx 类，
# 加载 BootActivity 会 NoClassDefFoundError 直接闪退。它用到的父类方法
# (onCreate/onResume/onPause/onDestroy/dispatchKeyEvent/onLowMemory/
#  onWindowFocusChanged) 在 android.app.Activity 里全部存在，改回 Activity 后生效。
find "$WORK/glue" -name '*.smali' -exec \
  sed -i 's|Landroidx/appcompat/app/AppCompatActivity;|Landroid/app/Activity;|g' {} +
if grep -rq "androidx/appcompat" "$WORK/glue"; then
  echo "  !! 仍有 androidx 残留，停止构建"; exit 1
fi
echo "  SDLActivity 父类已改为 android.app.Activity（消除启动闪退）"
# 修正跨包访问（1.0.4）：Limbo 反编译出的 SDLSurface 是包私有类
# （.class 行没有 public），而自研 BootActivity 在 dev.aether.vm 包，
# 直接 new SDLSurface(this) 会抛 IllegalAccessError。SDL2 官方源码里
# SDLSurface 本来就是 public，这里改回 public，仅供本工程跨包实例化。
sed -i 's|^\.class Lorg/libsdl/app/SDLSurface;|.class public Lorg/libsdl/app/SDLSurface;|' \
  "$WORK/glue/org/libsdl/app/SDLSurface.smali"
if ! grep -q "^\.class public Lorg/libsdl/app/SDLSurface;" \
     "$WORK/glue/org/libsdl/app/SDLSurface.smali"; then
  echo "  !! SDLSurface 访问标志修正失败，停止构建"; exit 1
fi
echo "  SDLSurface 已改回 public（消除跨包 IllegalAccessError）"
smali a "$WORK/glue" -o "$OUT/classes2.dex"
echo "  glue 类数: $(find "$WORK/glue" -name '*.smali' | wc -l)"

echo "=== [6/9] 交叉编译自研 JNI 库 libaethervm.so（arm64）==="
$NDK -shared -fPIC -O2 -I"$R/core" -I"$A/jni" \
  "$R/core/vm_config.c" "$R/core/vm_runtime.c" \
  "$A/jni/aether_vm_jni.c" "$A/jni/crash_signal.c" \
  -o "$OUT/lib/arm64-v8a/libaethervm.so" -llog -ldl
file "$OUT/lib/arm64-v8a/libaethervm.so" | cut -c1-120
llvm-readelf -Ws "$OUT/lib/arm64-v8a/libaethervm.so" 2>/dev/null \
  | grep -E "Java_dev_aether_vm_CrashGuard_nativeInstall" | head -1

echo "=== [7/9] 放入引擎与依赖 .so（开源二进制，照搬）==="
SRC_LIB=/tmp/jnilib-extract/lib/arm64-v8a
for f in libcompat-limbo.so libcompat-musl.so libcompat-SDL2-addons.so \
         libcompat-SDL2-ext.so libglib-2.0.so libpixman-1.so libSDL2.so; do
  cp "$SRC_LIB/$f" "$OUT/lib/arm64-v8a/$f"
done
cp "$ENGINE" "$OUT/lib/arm64-v8a/libqemu-system-aarch64.so"
echo "  native 库:"; ls -1 "$OUT/lib/arm64-v8a/"

echo "=== [8/9] 放入内置资产与配置 ==="
cp "$KERNEL" "$OUT/assets/kernel.img"
cp "$DISK"   "$OUT/assets/system.qcow2"
cp "$A/assets/vm.conf" "$OUT/assets/vm.conf"

echo "=== [9/9] 组装 APK → zipalign → 签名 ==="
python3 - <<'PY'
import zipfile, os
base = "/workspace/aether-vm/build/work/base.apk"
out  = "/workspace/aether-vm/build/work/unsigned.apk"
root = "/workspace/aether-vm/out/apk"
zin = zipfile.ZipFile(base, "r")
zout = zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6)
for it in zin.infolist():
    zi = zipfile.ZipInfo(it.filename, date_time=it.date_time)
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = it.external_attr
    zout.writestr(zi, zin.read(it.filename))
zin.close()
for dirpath, _, files in os.walk(root):
    for f in files:
        full = os.path.join(dirpath, f)
        arc  = os.path.relpath(full, root)
        zi = zipfile.ZipInfo(arc.replace(os.sep, "/"))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        zout.writestr(zi, open(full, "rb").read())
zout.close()
print("  unsigned.apk 生成，条目数:", len(zipfile.ZipFile(out).namelist()))
PY
$BT/zipalign -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
FINAL=/workspace/AetherVM.apk
$BT/apksigner sign --ks "$KS" --ks-key-alias aetheros \
  --ks-pass "pass:$KSPW" --key-pass "pass:$KSPW" \
  --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
  --out "$FINAL" "$WORK/aligned.apk"

echo ""
echo "=== 签名验证 ==="
$BT/apksigner verify -v "$FINAL" 2>&1 | grep -E "Verifies|scheme"
echo ""
echo "=== 崩溃监测器 + 黑屏修复链路自检（字节码级） ==="
echo "--- dex 里的自研类 ---"
unzip -p "$FINAL" classes.dex > "$WORK/peek.dex"
strings "$WORK/peek.dex" | grep -oE "Ldev/aether/vm/[A-Za-z]+;" | sort -u | sed 's|^|  |'
echo "--- 反编译 BootActivity：确认 Surface 补齐 + 引擎走 SDL2 官方 nativeRunMain ---"
rm -rf "$WORK/vdex" && mkdir -p "$WORK/vdex"
baksmali d "$WORK/peek.dex" -o "$WORK/vdex" 2>/dev/null
grep -nE "nativeRunMain|setContentView|mSurface|SDLSurface|scheduleEngineFallback|mExitCalledFromJava|captureStdio|DiagnosticsOverlay|CrashHandler;->markStage" \
  "$WORK/vdex/dev/aether/vm/BootActivity.smali" 2>/dev/null | sed 's|^|  |'
echo "--- 1.0.5 零反馈修复链路：阶段标记 / 冷启动合成 / 内联报告 / crash buffer ---"
echo "  阶段打点数（BootActivity）: $(grep -c 'CrashHandler;->markStage' "$WORK/vdex/dev/aether/vm/BootActivity.smali" 2>/dev/null)"
grep -q "synthesizeOnColdStart" "$WORK/vdex/dev/aether/vm/CrashHandler.smali" 2>/dev/null \
  && echo "  [OK] CrashHandler.synthesizeOnColdStart 存在" \
  || { echo "  !! 缺少冷启动报告合成"; exit 1; }
grep -q "STAGE_NATIVE_RUNMAIN_ENTER" "$WORK/vdex/dev/aether/vm/BootActivity.smali" 2>/dev/null \
  && echo "  [OK] 进入 QEMU native 前有阶段打点" \
  || { echo "  !! 缺少 native 入口打点"; exit 1; }
grep -q "showReportPage" "$WORK/vdex/dev/aether/vm/LaunchActivity.smali" 2>/dev/null \
  && echo "  [OK] LaunchActivity 内联报告页存在（不依赖跨进程弹窗）" \
  || { echo "  !! 缺少内联报告页"; exit 1; }
grep -q "main,system,crash" "$WORK/vdex/dev/aether/vm/CrashHandler.smali" 2>/dev/null \
  && echo "  [OK] logcat 抓取含 crash buffer（native tombstone）" \
  || { echo "  !! logcat 未抓 crash buffer"; exit 1; }
grep -q "qemu_stdio.log" "$WORK/vdex/dev/aether/vm/CrashHandler.smali" 2>/dev/null \
  && echo "  [OK] 冷启动报告已纳入 QEMU stdout/stderr" \
  || { echo "  !! 报告缺引擎输出"; exit 1; }
echo "--- 编译期 stub 不得进入 classes.dex（真实 SDL glue 只在 classes2.dex）---"
if [ -d "$WORK/vdex/org/libsdl/app" ]; then
  echo "  !! stub 类被误打包，运行时会与 SDL glue 类冲突"; exit 1
else
  echo "  classes.dex 仅含对 org.libsdl.app 的引用，无 stub 定义 OK"
fi
echo "--- VmEngine JNI 符号（captureStdio / start / stop）---"
llvm-readelf -Ws "$OUT/lib/arm64-v8a/libaethervm.so" 2>/dev/null \
  | grep -oE "Java_dev_aether_vm_(VmEngine_captureStdio|VmEngine_start|VmConfig_buildArgs)" \
  | sort -u | sed 's|^|  |'
echo "--- Manifest 版本与声明 ---"
$BT/aapt dump badging "$FINAL" 2>/dev/null | grep -E "^package:|launchable-activity" | sed 's|^|  |'
echo "--- android/assets/vm.conf 关键参数（真机稳健值）---"
grep -E "^(accel|display|engine|backend|memory_mb|smp)" "$A/assets/vm.conf" | sed 's|^|  |'
grep -q "^accel       = tcg,tb-size=128" "$A/assets/vm.conf" || { echo "  !! accel 未用兼容性最好的 tcg,tb-size=128"; exit 1; }
grep -q "memory_mb   = 384" "$A/assets/vm.conf" || { echo "  !! guest 内存未下调（LMK 风险）"; exit 1; }
echo "--- SDLActivity 父类（应为 android.app.Activity）---"
unzip -p "$FINAL" classes2.dex > "$WORK/peek2.dex"
strings "$WORK/peek2.dex" | grep -oE "Landroidx/appcompat/app/AppCompatActivity;|Landroid/app/Activity;" \
  | sort -u | sed 's|^|  |'
echo "--- classes2.dex 类访问标志（跨包引用的两个类必须 public）---"
rm -rf "$WORK/gluev" && mkdir -p "$WORK/gluev"
baksmali d "$WORK/peek2.dex" -o "$WORK/gluev" 2>/dev/null
grep -h "^\.class " "$WORK/gluev/org/libsdl/app/SDLSurface.smali" \
  "$WORK/gluev/org/libsdl/app/SDLActivity.smali" | sed 's|^|  |'
grep -q "^\.class public Lorg/libsdl/app/SDLSurface;" \
  "$WORK/gluev/org/libsdl/app/SDLSurface.smali" || {
    echo "  !! classes2.dex 里 SDLSurface 仍非 public，真机会 IllegalAccessError"; exit 1; }
echo "--- 全量跨包类扫描：classes.dex 引用的所有 glue 类必须 public ---"
AETHER_VDEX="$WORK/vdex" AETHER_GLUEV="$WORK/gluev" python3 - <<'PY'
import re, glob, os, sys
vd, gd = os.environ["AETHER_VDEX"], os.environ["AETHER_GLUEV"]
refs = set()
for f in glob.glob(vd + "/dev/aether/vm/*.smali"):
    for m in re.findall(r'Lorg/libsdl/app/([^;\[]+);', open(f, errors="ignore").read()):
        refs.add(m)
bad = []
for r in sorted(refs):
    p = os.path.join(gd, "org/libsdl/app", r + ".smali")
    line = ""
    if os.path.exists(p):
        line = next((l.strip() for l in open(p, errors="ignore") if l.startswith(".class ")), "")
    if not line.startswith(".class public "):
        bad.append(r)
print("  自研 dex 跨包引用 %d 个 glue 类，非 public: %s" % (len(refs), bad or "无"))
if bad: sys.exit(1)
PY
echo "--- 1.0.6 报告结构自检：关键三段置顶 + native 摘要 + BootTrace 裁剪 ---"
grep -q "nativeCrashDigest" "$WORK/vdex/dev/aether/vm/CrashHandler.smali" 2>/dev/null \
  && echo "  [OK] native 崩溃摘要提取已打包" \
  || { echo "  !! 缺 native 崩溃摘要"; exit 1; }
grep -q "describeTail" "$WORK/vdex/dev/aether/vm/CrashHandler.smali" 2>/dev/null \
  && echo "  [OK] 日志字节数标注已打包" \
  || { echo "  !! 缺日志字节数标注"; exit 1; }
grep -q "trimHistory" "$WORK/vdex/dev/aether/vm/BootTrace.smali" 2>/dev/null \
  && echo "  [OK] BootTrace 历史已裁剪（报告不会被撑爆）" \
  || { echo "  !! BootTrace 未裁剪"; exit 1; }
python3 - <<PY
p = "$A/java/dev/aether/vm/CrashHandler.java"
s = open(p, encoding="utf-8", errors="ignore").read()
i = s.find("★ ① 引擎 stdout/stderr"); j = s.find("启动进度 BootTrace")
if i < 0 or j < 0 or i > j:
    raise SystemExit("  !! 关键三段未排在 BootTrace 之前")
print("  [OK] 关键三段在报告中排在 BootTrace 之前（防粘贴截断）")
PY
echo ""
echo "--- 1.0.7 保存文件改造自检：剪贴板已移除 / 落盘与分享链路在位 ---"
echo "  [dex] 自研类应含 ReportSaver / ReportProvider："
strings "$WORK/peek.dex" | grep -oE "Ldev/aether/vm/(ReportSaver|ReportProvider);" \
  | sort -u | sed 's|^|    |'
for cls in ReportSaver ReportProvider; do
  grep -q "Ldev/aether/vm/$cls;" "$WORK/peek.dex" 2>/dev/null \
    || { strings "$WORK/peek.dex" | grep -q "$cls" || { echo "  !! 缺 $cls"; exit 1; }; }
done
echo "  [OK] ReportSaver / ReportProvider 已打包进 classes.dex"
echo "--- 全工程不得再有 ClipboardManager / setPrimaryClip（1.0.7 起停用剪贴板）---"
if grep -rqE "ClipboardManager|ClipData|setPrimaryClip|复制全部" "$A/java"; then
  echo "  !! 仍存在剪贴板代码或「复制全部」字样："
  grep -rnE "ClipboardManager|ClipData|setPrimaryClip|复制全部" "$A/java" | sed 's|^|    |'
  exit 1
else
  echo "  [OK] Java 源码已无剪贴板 / 复制全部残留"
fi
echo "--- 三个界面都调用 ReportSaver.saveAndShare（含匿名内部类 $1）---"
for cls in LaunchActivity CrashReportActivity DiagnosticsOverlay; do
  if grep -lq "ReportSaver;->saveAndShare" \
       "$WORK/vdex/dev/aether/vm/$cls.smali" \
       "$WORK/vdex/dev/aether/vm/$cls"\$*.smali 2>/dev/null; then
    echo "    [OK] $cls 类族 -> saveAndShare"
  else
    echo "  !! $cls 类族未调用 saveAndShare"; exit 1
  fi
done
echo "--- 报告合成/崩溃时自动落盘 ---"
grep -q "ReportSaver;->save" "$WORK/vdex/dev/aether/vm/CrashHandler.smali" 2>/dev/null \
  && echo "    [OK] CrashHandler 自动落盘 makeReport 文件" \
  || { echo "  !! CrashHandler 未自动落盘"; exit 1; }
echo "--- Manifest 已注册 ReportProvider（authorities / grantUriPermissions）---"
grep -q "dev.aether.vm.reports" "$A/AndroidManifest.xml" \
  && grep -q "grantUriPermissions=\"true\"" "$A/AndroidManifest.xml" \
  && echo "    [OK] Provider 已声明且可授权 URI" \
  || { echo "  !! Provider 声明缺失"; exit 1; }
echo "--- 报告内不再出现「粘贴不下/复制以上」类截断提示 ---"
if grep -qE "粘贴不下|复制以上全部|优先发下面" "$A/java/dev/aether/vm/CrashHandler.java"; then
  echo "  !! 仍留有粘贴截断提示文案"; exit 1
else
  echo "  [OK] 文案已改为「完整文件直发」"
fi
echo ""
echo "--- 1.0.8 引擎修复自检：strchrnul 隐式声明导致的 64 位指针截断已消除 ---"
rm -rf "$WORK/enginechk" && mkdir -p "$WORK/enginechk"
unzip -o -q "$FINAL" lib/arm64-v8a/libqemu-system-aarch64.so -d "$WORK/enginechk"
ENGSO="$WORK/enginechk/lib/arm64-v8a/libqemu-system-aarch64.so"
echo "  APK 内引擎大小: $(stat -c %s "$ENGSO") B"
if aarch64-linux-gnu-nm -D "$ENGSO" 2>/dev/null | grep -qi 'strchrnul'; then
  echo "  !! 引擎仍引用 strchrnul（隐式声明会截断 64 位指针）"; exit 1
else
  echo "    [OK] 引擎动态符号中 strchrnul 引用 = 0"
fi
CUTCNT=$(aarch64-linux-gnu-objdump -d "$ENGSO" 2>/dev/null \
  | grep -A1 'bl.*<strchrnul@plt>' | grep -c 'sxtw' || true)
if [ "$CUTCNT" -ne 0 ]; then
  echo "  !! 引擎仍存在 sxtw 32 位符号扩展截断（$CUTCNT 处）"; exit 1
else
  echo "    [OK] 参数解析路径无 sxtw 符号扩展截断"
fi
aarch64-linux-gnu-nm -D "$ENGSO" 2>/dev/null | grep -q ' T SDL_main$' \
  && echo "    [OK] SDL_main 入口仍导出（nativeRunMain 可驱动）" \
  || { echo "  !! 缺 SDL_main 导出"; exit 1; }
echo ""
echo "--- 1.0.9 显示修复自检：客机画面铺满屏幕 + 输入坐标反算 + 开机动画/去鼠标 ---"
if aarch64-linux-gnu-nm -D "$ENGSO" 2>/dev/null | grep -q ' T sdl2_get_present_rect'; then
  echo "    [OK] 引擎含 sdl2_get_present_rect（contain 缩放，画面铺满屏幕）"
else
  echo "  !! 引擎缺少 sdl2_get_present_rect（画面会缩在角落）"; exit 1
fi
# 阶段 6 硬校验：APK 内 system.qcow2 必须与 canonical v6 盘逐字节一致，
# 且盘内 /sbin/aether-compositor 必须与本次 root-full 编译产物一致。
# 用 debugfs 直接从 raw 读出，不依赖沙箱内不可用的 guestmount / loop 挂载。
COMP=/root/aether-archive/aether-aarch64-build/root-full/sbin/aether-compositor
rm -rf "$WORK/gchk" && mkdir -p "$WORK/gchk"
unzip -o -q "$FINAL" assets/system.qcow2 -d "$WORK/gchk"
APK_DISK="$WORK/gchk/assets/system.qcow2"
if ! cmp -s "$APK_DISK" "$DISK"; then
  echo "  !! APK 内 system.qcow2 与 canonical v6 盘不一致"; exit 1
fi
echo "    [OK] APK 内 system.qcow2 与 canonical v6 盘逐字节一致 ($(stat -c %s "$APK_DISK") B)"
qemu-img convert -f qcow2 -O raw "$APK_DISK" "$WORK/gchk/root.raw" 2>/dev/null
rm -f "$WORK/gchk/injected.bin"
debugfs -R "dump /sbin/aether-compositor $WORK/gchk/injected.bin" "$WORK/gchk/root.raw" >/dev/null 2>&1
if [ ! -s "$WORK/gchk/injected.bin" ]; then
  echo "  !! 无法从 APK 内系统盘读出 aether-compositor"; exit 1
fi
if ! cmp -s "$WORK/gchk/injected.bin" "$COMP"; then
  echo "  !! APK 内系统盘的 compositor 与本次编译产物不一致"; exit 1
fi
echo "    [OK] 盘内 compositor 与 root-full 编译产物逐字节一致 ($(stat -c %s "$COMP") B)"
if ! e2fsck -fn "$WORK/gchk/root.raw" >/dev/null 2>&1; then
  echo "  !! APK 内系统盘 ext4 元数据不干净"; exit 1
fi
echo "    [OK] APK 内系统盘 e2fsck -fn 干净（journal 已固化）"
echo ""
echo "--- 1.0.10 运行时符号完整性自检（防止头文件/运行时 SDL2 版本错配导致 dlopen 失败）---"
# 交叉编译用的 SDL2 头文件为 2.30.0，而 APK 内置的 libSDL2.so 更旧；
# 直接调用头文件里声明、运行时库里未导出的 API 会让 dlopen 在真机上直接失败。
# 这里把「引擎引用的 SDL/扩展库符号」与「APK 内全部 so 实际导出的符号」做差集。
rm -rf "$WORK/symchk" && mkdir -p "$WORK/symchk"
unzip -o -q "$FINAL" 'lib/arm64-v8a/*.so' -d "$WORK/symchk"
aarch64-linux-gnu-nm -D --defined-only "$WORK"/symchk/lib/arm64-v8a/*.so 2>/dev/null \
  | awk 'NF>=3{print $3}' | sort -u > "$WORK/symchk/defined.txt"
aarch64-linux-gnu-nm -D -u "$ENGSO" 2>/dev/null \
  | awk '{print $NF}' | sort -u > "$WORK/symchk/undef.txt"
MISSING=$(grep -E '^(SDL_|IMG_|TTF_|Mix_)' "$WORK/symchk/undef.txt" \
  | grep -vxF -f "$WORK/symchk/defined.txt" || true)
if [ -n "$MISSING" ]; then
  echo "  !! 引擎引用了 APK 内运行时库未导出的符号，真机 dlopen 必然失败："
  echo "$MISSING" | sed 's/^/       /'
  exit 1
else
  echo "    [OK] 引擎引用的 SDL/扩展库符号全部由 APK 内运行时库提供"
fi
if aarch64-linux-gnu-nm -D "$ENGSO" 2>/dev/null | grep -q 'SDL_SetTextureScaleMode'; then
  echo "  !! 引擎仍直接引用 SDL_SetTextureScaleMode（运行时库未导出）"; exit 1
else
  echo "    [OK] 引擎不再引用 SDL_SetTextureScaleMode，改用兼容 hint"
fi
echo ""
echo "--- 1.0.11 屏幕尺寸注入自检（画面不再缩在角落）---"
# Java 层：AETHER_SCREEN 环境变量注入必须真的进入 classes.dex
rm -rf "$WORK/vdex11"
baksmali d "$WORK/peek.dex" -o "$WORK/vdex11" 2>/dev/null
if grep -q 'AETHER_SCREEN' "$WORK/vdex11/dev/aether/vm/AetherApp.smali" 2>/dev/null \
   && grep -q 'setenv' "$WORK/vdex11/dev/aether/vm/AetherApp.smali" 2>/dev/null; then
  echo "    [OK] classes.dex 含 AETHER_SCREEN 注入与 Os.setenv"
else
  echo "  !! classes.dex 缺少屏幕尺寸注入（真机仍会拿到 guest 小尺寸）"; exit 1
fi
# 引擎层：多源尺寸诊断与 contain 缩放 helper 必须真的进入 so
if strings "$ENGSO" | grep -q 'AETHER_SDL present:'; then
  echo "    [OK] 引擎含多源尺寸诊断（injected / renderer / window / desktop）"
else
  echo "  !! 引擎缺少 AETHER_SDL present 诊断"; exit 1
fi
if aarch64-linux-gnu-nm -D "$ENGSO" 2>/dev/null | grep -q ' T sdl2_get_present_rect'; then
  echo "    [OK] sdl2_get_present_rect 已导出（contain 等比缩放生效）"
else
  echo "  !! sdl2_get_present_rect 未导出（画面会缩在角落）"; exit 1
fi
echo ""
echo "=== 产物 ==="
ls -lh "$FINAL"
