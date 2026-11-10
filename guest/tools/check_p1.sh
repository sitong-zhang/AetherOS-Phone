#!/usr/bin/env bash
# check_p1.sh - V3.0 分册 P1「材质与动效体系」静态核查 + 单元断言
#
# 检查项对应分册 §7 / §8 / §10.9.3 / §10.10 / §16 C/D/E 组 / §17 P1 五项：
#   M1–M12 配方表、模糊复用与降采样（E02）
#   五曲线 + 时长阶梯 + 弹簧 + 可打断续接（C02）
#   T-01/T-02 图标共享转场、T-05 右滑返回、曲线时长统一
#   全局按压提亮与膨胀（C01）
#   D03 两段式通知中心 240/640/1280、D04 控制中心二级面板
#   顺带修复开机动画品牌字右缘裁切
#
# 不产生任何对外发布物，只在本地输出 PASS / FAIL 清单。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"
OUT="${AETHER_BUILD:-/root/aether-archive/aether-pc-build/root}"
mkdir -p "$OUT/bin"
PASS=0
FAIL=0

ok()   { printf '  PASS  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL+1)); }
cond() { if [ "$1" = "0" ]; then ok "$2"; else bad "$2"; fi; }
has()  { grep -qE "$2" "$SRC/$1" 2>/dev/null; }
# 只统计代码行：过滤注释行，注释里的规范文字不算命中
code_hits() {
    grep -rnE "$1" "$SRC"/*.c "$SRC"/*.h 2>/dev/null \
      | grep -v 'stb_truetype' \
      | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true
}

echo "=== [1/8] M1–M12 配方表 / 同帧模糊预算（分册 §7.2 / E02） ==="
for f in aether-material.h aether-material.c; do
    cond "$(has "$f" 'AE_M12_ISLAND' && echo 0 || echo 1)" "$f 定义/实现 M1–M12 全表"
done
N=$(grep -c 'AE_M[0-9]' "$SRC/aether-material.h" || true)
cond "$([ "$N" -ge 12 ] && echo 0 || echo 1)" "材质枚举覆盖 12 种表面（$N 处引用）"
cond "$(has aether-material.c 'RECIPES\[AE_MAT_COUNT\]' && echo 0 || echo 1)" "配方为表驱动（非散落 if）"
for sym in aether_material_begin_frame aether_material_request_blur aether_material_blur_budget; do
    cond "$(has aether-material.c "$sym" && echo 0 || echo 1)" "材质层提供 $sym"
done
cond "$(has aether-phone.c 'aether_material_request_blur' && echo 0 || echo 1)" "浮层背景登记本帧唯一一次真算模糊"
cond "$(has aether-phone.c 'aether_material_begin_frame' && echo 0 || echo 1)" "每帧开头清零模糊预算"
for f in build-host.sh build-aarch64.sh; do
    cond "$(grep -q 'aether-material.c' "$ROOT/$f" && echo 0 || echo 1)" "$f 链接 aether-material.c"
done

echo "=== [2/8] 五曲线 + 时长阶梯（分册 §8.1 / §8.3） ==="
for c in aether_ease_out aether_ease_in_out aether_ease_back aether_ease_elastic; do
    cond "$(has aether-anim.c "$c" && echo 0 || echo 1)" "曲线 $c 落地"
done
# C-LINEAR 是头文件里的 static inline（零调用开销），不住 .c 里
cond "$(has aether-anim.h 'aether_ease_linear' && echo 0 || echo 1)" "曲线 aether_ease_linear 落地（头文件 inline）"
for t in TK_D_INSTANT TK_D_FAST TK_D_BASE TK_D_MATERIALIZE TK_D_MOVE TK_D_DEMATERIALIZE TK_D_SPRING TK_D_SLOW; do
    cond "$(has aether-tokens.h "$t" && echo 0 || echo 1)" "时长令牌 $t 就位"
done
# 只查业务层源码；aether-tokens.h 里的 GLASS_*_MS 是「单一真相源」对上游
# 常量的映射别名，允许存在，不算业务层裸时长。
B=$(grep -nE 'GLASS_[A-Z_]+_MS' \
      "$SRC/aether-phone.c" "$SRC/aether-ui.c" "$SRC/aether-apps2.c" \
      "$SRC/aether-settings.c" "$SRC/aether-store.c" "$SRC/aether-island.c" \
      2>/dev/null | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true)
if [ -z "$B" ]; then ok "业务层不再直接写 GLASS_* 裸时长（统一走 TK_D_*）"; else bad "残留裸时长：$B"; fi
cond "$(has aether-phone.c 'TK_D_DEMATERIALIZE' && echo 0 || echo 1)" "浮层收起走 350ms 阶梯（非硬切）"

echo "=== [3/8] 可打断续接 / 弹簧 / 图标共享转场（§8.2 / §8.5 / C02） ==="
for sym in aether_anim_begin_from aether_anim_set aether_anim_visual aether_anim_duration; do
    cond "$(has aether-anim.c "$sym" && echo 0 || echo 1)" "动效层提供 $sym"
done
for sym in aether_spring_init aether_spring_to aether_spring_step aether_spring_running aether_spring_settle; do
    cond "$(has aether-anim.c "$sym" && echo 0 || echo 1)" "弹簧 API $sym"
done
M=$(grep -c 'aether_anim_begin_from' "$SRC/aether-phone.c" || true)
cond "$([ "$M" -ge 5 ] && echo 0 || echo 1)" "外壳 5+ 处转场走可打断续接（$M 处）"
cond "$(has aether-phone.c 'g_app_from_kind' && echo 0 || echo 1)" "T-01/T-02 记录共享元素来源图标"
cond "$(has aether-phone.c 'icon_a.*g_closing' && echo 0 || echo 1)" "图标 60% 淡出 / 40% 淡入交叉"
cond "$(has aether-phone.c 'aether_anim_visual\(&g_an_app' && echo 0 || echo 1)" "返回时从窗口当前展开度续接"

echo "=== [4/8] 两段式通知中心（D03 / T-08~T-10） ==="
cond "$(has aether-phone.c '#define NOTIF_SNAP1[[:space:]]+240' && echo 0 || echo 1)" "第一吸附点 240px"
cond "$(has aether-phone.c '#define NOTIF_SNAP2[[:space:]]+640' && echo 0 || echo 1)" "第二吸附点 640px"
for sym in notif_snap_points notif_nearest_stage notif_snap_to notif_full_h; do
    cond "$(has aether-phone.c "$sym" && echo 0 || echo 1)" "两段式提供 $sym"
done
cond "$(has aether-phone.c 'g_notif_pending' && echo 0 || echo 1)" "状态栏下拉 / 面板内拖拽跟手候选"
cond "$(has aether-phone.c 'aether_anim_set\(&g_an_panel, e\)' && echo 0 || echo 1)" "跟手 1:1 钉住展开度"
cond "$(has aether-phone.c 'int ph = g_notif_h' && echo 0 || echo 1)" "通知面板高度由展开度派生"
cond "$(has aether-phone.c 'cmd, "notif"' && echo 0 || echo 1)" "取证 IPC 支持三吸附点"

echo "=== [5/8] 控制中心二级面板（D04 / T-5） ==="
for sym in g_cc_detail cc_detail_geom cc_detail_open cc_detail_action; do
    cond "$(has aether-phone.c "$sym" && echo 0 || echo 1)" "二级面板提供 $sym"
done
cond "$(has aether-phone.c 'TK_LONGPRESS_MS' && echo 0 || echo 1)" "长按 400ms 触发（分册 §10.10）"
cond "$(has aether-phone.c 'AE_M10_SHEET' && echo 0 || echo 1)" "二级面板使用 M10 重模糊配方"
cond "$(has aether-phone.c 'longpress_tick' && echo 0 || echo 1)" "帧驱动 + up 兜底双路径长按判定"
cond "$(has aether-phone.c 'cmd, "ccdetail"' && echo 0 || echo 1)" "取证 IPC 支持二级面板"

echo "=== [6/8] T-05 右滑返回 / 全局按压（§8.5 / §7.7 / C01 / C08） ==="
cond "$(has aether-phone.c 'TK_EDGE_ZONE' && echo 0 || echo 1)" "左边缘 24px 返回热区"
cond "$(has aether-phone.c 'TK_EDGE_SYS' && echo 0 || echo 1)" "8px 物理边缘不触发控件"
cond "$(has aether-phone.c 'g_edge_x' && echo 0 || echo 1)" "右滑跟手位移状态"
cond "$(has aether-phone.c 'aether_ease_elastic' && echo 0 || echo 1)" "未达阈值按 C-ELASTIC 归位"
for sym in aether_material_press_lift aether_material_press_inflate aether_material_pressed; do
    cond "$(has aether-ui.c "$sym" && echo 0 || echo 1)" "应用内按钮接入 $sym"
done
P=$(grep -c 'aether_material_press_lift' "$SRC/aether-phone.c" || true)
cond "$([ "$P" -ge 3 ] && echo 0 || echo 1)" "外壳玻璃控件/磁贴接入材质提亮（$P 处）"
cond "$(has aether-ui.c 'aether-material.h' && echo 0 || echo 1)" "aether-ui.c 引用材质层头文件"

echo "=== [7/8] 开机字标裁切修复 + 编译产物符号 ==="
cond "$(has aether-render.c 'aether_text_width_scale' && echo 0 || echo 1)" "渲染层提供缩放后宽度"
cond "$(has aether-render.h 'aether_text_width_scale' && echo 0 || echo 1)" "缩放宽度 API 已导出"
cond "$(has aether-phone.c 'aether_text_width_scale\(logo' && echo 0 || echo 1)" "开机字标按缩放宽度居中"
if [ -f "$OUT/sbin/aether-compositor" ]; then
    for sym in aether_anim_begin_from aether_anim_set aether_spring_step aether_material_request_blur aether_material_press_lift aether_text_width_scale; do
        S=$(nm "$OUT/sbin/aether-compositor" 2>/dev/null | grep -c " $sym\$" || true)
        cond "$([ "$S" -ge 1 ] && echo 0 || echo 1)" "合成器二进制含符号 $sym"
    done
else
    bad "未找到 $OUT/sbin/aether-compositor（请先跑 build-host.sh）"
fi

echo "=== [8/8] C 单元断言（曲线 / 可打断 / 弹簧 / 配方 / 预算 / 按压 / 字标） ==="
FONT="$ROOT/assets/aether-font.ttf"
if [ -f "$FONT" ]; then
    OBJ="$OUT/bin/p1-font-blob.o"
    ( cd "$ROOT/assets" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        aether-font.ttf "$OBJ" )
    if gcc -O2 -Wall -Wextra -I"$SRC" -DAETHER_VECTOR_FONT \
        "$ROOT/tools/test_p1_motion.c" \
        "$SRC/aether-anim.c" "$SRC/aether-material.c" \
        "$SRC/aether-render.c" "$SRC/aether-vfont.c" "$OBJ" \
        -lm -o "$OUT/bin/test_p1_motion" 2>"$OUT/bin/test_p1_motion.build.log"; then
        ok "test_p1_motion.c 编译通过"
        if "$OUT/bin/test_p1_motion"; then ok "P1 单元断言全绿"; else bad "P1 单元断言存在 FAIL"; fi
    else
        bad "test_p1_motion.c 编译失败，日志：$OUT/bin/test_p1_motion.build.log"
        tail -25 "$OUT/bin/test_p1_motion.build.log"
    fi
else
    bad "缺少 $FONT，无法编译矢量字体断言"
fi

echo ""
echo "=== P1 核查结果：$PASS PASS / $FAIL FAIL ==="
[ "$FAIL" -eq 0 ]
