#!/usr/bin/env bash
# check_p0.sh - V3.0 分册 P0「结构纠偏」静态核查 + 单元断言
#
# 检查项对应分册 §16 A/B 组与 §17 P0 五项：
#   A01 指针/光标绘制路径为零（源码 + 编译产物符号双查）
#   A02 浮层创建路径调用 glass_role_allows_glass()
#   A04 业务层不再出现固定 16px 字高的居中公式；关键文件引用 aether-tokens.h
#   A07 标题栏 96 / 返回命中 88 令牌落地
#   B01 含 descender 文案上下 4px 带无残留（C 单元断言）
#   B03 溢出显示省略号且无半个 UTF-8 汉字（C 单元断言）
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

# 只统计代码行：过滤以 * / /* // 开头的注释行，注释里的说明文字不算命中
code_hits() {
    grep -rnE "$1" "$SRC"/*.c "$SRC"/*.h 2>/dev/null \
      | grep -v 'stb_truetype' \
      | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true
}

echo "=== [1/6] 指针 / 光标 / 相对鼠标事件路径（分册 A01） ==="
P=$(code_hits 'aether_draw_cursor|aether_render_set_cursor|cursor_x|cursor_y|BTN_LEFT|REL_X|REL_Y')
if [ -z "$P" ]; then ok "源码无指针绘制与鼠标相对位移事件"; else bad "源码残留：$P"; fi
if [ -f "$OUT/sbin/aether-compositor" ]; then
    # 只查本工程自有符号：host 构建静态链接 libdrm 会带进 drmModeSetCursor
    # 等系统库导出 API，那是 libdrm 的接口而非本工程的指针绘制路径，
    # 且 ARM64 fbdev 构建根本不链接 libdrm（见 build-aarch64.sh）。
    S=$(nm "$OUT/sbin/aether-compositor" 2>/dev/null \
        | grep -iE 'aether_[A-Za-z0-9_]*(cursor|hover)|draw_cursor|cursor_[xy]' || true)
    if [ -z "$S" ]; then ok "合成器二进制无本工程指针 / hover 符号"; else bad "二进制符号残留：$S"; fi
    U=$(nm -u "$OUT/sbin/aether-compositor" 2>/dev/null | grep -iE 'cursor|hover' || true)
    if [ -z "$U" ]; then ok "无未定义的指针相关外部调用"; else bad "存在外部指针调用：$U"; fi
else
    bad "未找到 $OUT/sbin/aether-compositor（请先跑 build-host.sh）"
fi

echo "=== [2/6] 玻璃准入判定（分册 A02） ==="
N=$(grep -c 'glass_role_allows_glass' "$SRC/aether-phone.c")
cond "$([ "$N" -ge 2 ] && echo 0 || echo 1)" "浮层创建路径调用 glass_role_allows_glass（$N 处）"

echo "=== [3/6] 单一参数真相源令牌接入（分册 A04 / 铁律五） ==="
for f in aether-phone.c aether-ui.c aether-apps2.c aether-settings.c aether-store.c aether-render.c aether-island.c; do
    cond "$(grep -q 'aether-tokens.h' "$SRC/$f" && echo 0 || echo 1)" "$f 引用 aether-tokens.h"
done
for f in aether-apps2.c aether-settings.c aether-store.c; do
    cond "$(grep -q 'TK_TITLEBAR_H' "$SRC/$f" && echo 0 || echo 1)" "$f 标题栏取 TK_TITLEBAR_H(96)"
done
if [ -f "$SRC/aether-tokens.h" ]; then ok "aether-tokens.h 存在"; else bad "aether-tokens.h 缺失"; fi
D=$(code_hits '#define[[:space:]]+TOPBAR|#define[[:space:]]+TITLEBAR|#define[[:space:]]+HEAD_H[[:space:]]+[0-9]')
if [ -z "$D" ]; then ok "TOPBAR / TITLEBAR / HEAD_H 数值宏已清除"; else bad "残留裸宏：$D"; fi

echo "=== [4/6] 文字基线公式（分册 A04 / B01） ==="
F=$(code_hits '\([hH][a-zA-Z_]*[[:space:]]*-[[:space:]]*16\)[[:space:]]*/[[:space:]]*2')
if [ -z "$F" ]; then ok "业务层无固定 16px 字高居中公式"; else bad "残留固定居中：$F"; fi
for sym in aether_text_line_h aether_text_box_y aether_draw_text_utf8_box aether_draw_text_utf8_sized; do
    cond "$(grep -q "$sym" "$SRC/aether-render.c" && echo 0 || echo 1)" "render 层提供 $sym"
done

echo "=== [5/6] 命中区与壁纸图层（分册 A07 / B06） ==="
cond "$(grep -q 'aether_ui_hit_min' "$SRC/aether-phone.c" && echo 0 || echo 1)" "外壳导航键走 88×88 最小命中"
cond "$(grep -q 'aether_ui_hit_min' "$SRC/aether-store.c" && echo 0 || echo 1)" "商店顶栏返回走 88×88 最小命中"
cond "$(grep -q 'wall_avg_luma' "$SRC/aether-phone.c" && echo 0 || echo 1)" "壁纸亮度自适应遮罩存在"
cond "$(grep -q 'build_wall_layer' "$SRC/aether-phone.c" && echo 0 || echo 1)" "壁纸+遮罩预合成背景层存在"
cond "$(grep -q 'TK_WALL_SCRIM_LOCK' "$SRC/aether-phone.c" && echo 0 || echo 1)" "锁屏壁纸遮罩令牌落地（不再纯渐变）"

echo "=== [6/6] C 单元断言（B01 / B03 / §5.3） ==="
FONT="$ROOT/assets/aether-font.ttf"
if [ -f "$FONT" ]; then
    OBJ="$OUT/bin/p0-font-blob.o"
    ( cd "$ROOT/assets" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        aether-font.ttf "$OBJ" )
    if gcc -O2 -Wall -Wextra -I"$SRC" -DAETHER_VECTOR_FONT \
        "$ROOT/tools/test_p0_text.c" "$SRC/aether-render.c" "$SRC/aether-vfont.c" \
        "$OBJ" -lm -o "$OUT/bin/test_p0_text" 2>"$OUT/bin/test_p0_text.build.log"; then
        ok "test_p0_text.c 编译通过"
        if "$OUT/bin/test_p0_text"; then ok "单元断言全绿"; else bad "单元断言存在 FAIL"; fi
    else
        bad "test_p0_text.c 编译失败，日志：$OUT/bin/test_p0_text.build.log"
        tail -20 "$OUT/bin/test_p0_text.build.log"
    fi
else
    bad "缺少 $FONT，无法编译矢量字体断言"
fi

echo ""
echo "=== P0 核查结果：$PASS PASS / $FAIL FAIL ==="
[ "$FAIL" -eq 0 ]
