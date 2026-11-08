#!/usr/bin/env python3
"""gen_cjk_font.py - 生成 AetherOS 的 16x16 中文点阵字体子集。

来源可溯：从宿主机已安装的 Noto Sans CJK SC（/usr/share/fonts/opentype/noto/
NotoSansCJK-Regular.ttc）经 Pillow 真实光栅化得到，非手绘猜测。每个汉字在
16x16 单元内按墨迹包围盒居中。输出 aether-font-cjk.h：

  - aether_cjk_cp[]     : 升序 codepoint 表（供二分查找）
  - aether_cjk_glyphs[][32] : 每字 16 行 x 2 字节，MSB 为最左像素

这是「覆盖 OS UI 中文字符的精选子集」，不是完整 CJK 字体——与 ASCII 字体只含
95 个可打印字符同理，属诚实的范围声明。要扩字符集，改 CHARS 即可重生成。

用法: python3 tools/gen_cjk_font.py > aether-font-cjk.h
"""
import sys
from PIL import Image, ImageDraw, ImageFont

TTC = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
SIZE = 16
INDEX = 2  # Noto Sans CJK 的 SC 字面（各字面共享汉字，Han  unification 下字形一致）

# 中文 UI 字符串用到的字符 + 一份常用字精选子集，便于后续 UI 复用。
CHARS = (
    "图形合成器窗口系统已就绪"          # 桌面中文状态行
    "中国移动互联网操作系统"            # 系统名/品牌词
    "显示文字开发者"                    # 功能词
    "你好世界"                          # 问候/示例
    "一二三四五六七八九十"              # 数字
    "人民大学习工作电时间年月日"        # 常用实词
    "山水风火光"                        # 自然/视觉词
    "系统更新检查立即"                  # P5「系统更新」应用 UI
)

# 去重保序
seen, chars = set(), []
for c in CHARS:
    if c not in seen:
        seen.add(c)
        chars.append(c)


def raster(ch):
    img = Image.new("1", (SIZE * 2, SIZE * 2), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=fnt, fill=1)
    bb = img.getbbox()
    if bb is None:
        return None  # 缺字
    gw, gh = bb[2] - bb[0], bb[3] - bb[1]
    ox = (SIZE - gw) // 2 - bb[0]
    oy = (SIZE - gh) // 2 - bb[1]
    cell = Image.new("1", (SIZE, SIZE), 0)
    cell.paste(img, (ox, oy))
    flat = list(cell.getdata())
    rows = []
    for y in range(SIZE):
        b = 0
        for x in range(SIZE):
            if flat[y * SIZE + x]:
                b |= 1 << (15 - x)  # MSB = 最左像素
        rows.append(b)
    return rows


def main():
    global fnt
    fnt = ImageFont.truetype(TTC, SIZE, index=INDEX)
    glyphs = {}
    missing = []
    for ch in chars:
        rows = raster(ch)
        if rows is None:
            missing.append(ch)
            continue
        glyphs[ord(ch)] = rows

    items = sorted(glyphs.items())  # 按 codepoint 升序
    n = len(items)

    out = []
    out.append("/*\n")
    out.append(" * aether-font-cjk.h - 16x16 CJK bitmap font subset.\n")
    out.append(" * Generated from Noto Sans CJK SC (NotoSansCJK-Regular.ttc)\n")
    out.append(" * at size %d via tools/gen_cjk_font.py (Pillow rasterization).\n" % SIZE)
    out.append(" * Each glyph: 16 scanlines x 2 bytes, MSB = leftmost pixel.\n")
    out.append(" * Sorted by codepoint for binary search. This is a curated\n")
    out.append(" * subset covering the OS UI Chinese strings, not a full CJK font.\n")
    out.append(" */\n")
    out.append("#ifndef AETHER_FONT_CJK_H\n#define AETHER_FONT_CJK_H\n\n")
    out.append("#include <stdint.h>\n\n")
    out.append("#define AETHER_CJK_W 16u\n#define AETHER_CJK_H 16u\n")
    out.append("#define AETHER_CJK_N %d\n\n" % n)

    out.append("static const uint32_t aether_cjk_cp[AETHER_CJK_N] = {\n    ")
    out.append(", ".join("0x%04X" % cp for cp, _ in items))
    out.append("\n};\n\n")

    out.append("static const uint8_t aether_cjk_glyphs[AETHER_CJK_N][32] = {\n")
    for cp, rows in items:
        ch = chr(cp)
        disp = ch if ch not in "\\\"'" else {"\\": "\\\\", "\"": "\\\"", "'": "\\'"}.get(ch, ch)
        out.append("    { /* U+%04X %s */\n" % (cp, disp))
        for y in range(SIZE):
            b = rows[y]
            out.append("        0x%02X, 0x%02X,\n" % ((b >> 8) & 0xFF, b & 0xFF))
        out.append("    },\n")
    out.append("};\n\n#endif /* AETHER_FONT_CJK_H */\n")

    sys.stdout.write("".join(out))
    sys.stderr.write("cjk glyphs: %d, missing: %s\n" % (n, missing or "none"))


if __name__ == "__main__":
    main()
