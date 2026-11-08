#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font_cjk.py - 从源码中提取所有非 ASCII 字符，生成 16x16 点阵字库。

字体来源：系统自带 Noto Sans CJK SC（SIL Open Font License），
           /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc index=2。
输出：src/aether-font-cjk.h，包含升序码点表 + 点阵 + 二分查找函数。
每个字形 16 行 x 16 位 = 32 字节，MSB 为最左像素。
"""
import glob
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
OUT = os.path.join(SRC, "aether-font-cjk.h")
FONT_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_INDEX = 2
FONT_SIZE = 16
THRESHOLD = 110


# 生成物/纯数据头：含中文语义注释或词表，不是界面文案，排除以免污染字库
SKIP = {
    "aether-font-cjk.h",       # 生成物自身
    "aether-icons-data.h",     # 图标语义注释
    "aether-icons.h",          # 图标 API 注释（含中文说明）
}

def collect():
    chars = set()
    for f in sorted(glob.glob(os.path.join(SRC, "*.[ch]"))):
        if os.path.basename(f) in SKIP:
            continue
        try:
            text = open(f, encoding="utf-8").read()
        except (OSError, UnicodeDecodeError):
            continue
        for ch in text:
            cp = ord(ch)
            if cp >= 0x80 and cp != 0xFFFD and not (0xD800 <= cp <= 0xDFFF):
                chars.add(cp)
    return sorted(chars)


def render_glyph(font, cp):
    img = Image.new("L", (16, 16), 0)
    d = ImageDraw.Draw(img)
    d.text((0, -1), chr(cp), font=font, fill=255)
    out = bytearray(32)
    px = img.load()
    for y in range(16):
        bits = 0
        for x in range(16):
            if px[x, y] >= THRESHOLD:
                bits |= 0x8000 >> x
        out[y * 2] = (bits >> 8) & 0xFF
        out[y * 2 + 1] = bits & 0xFF
    return out


def main():
    cps = collect()
    if not cps:
        raise SystemExit("没有收集到字符，请检查源码目录")
    font = ImageFont.truetype(FONT_PATH, FONT_SIZE, index=FONT_INDEX)
    glyphs = [render_glyph(font, cp) for cp in cps]

    lines = []
    lines.append("/*")
    lines.append(" * aether-font-cjk.h - 16x16 CJK / 符号点阵字库（tools/gen_font_cjk.py 生成）")
    lines.append(" * 字体来源：Noto Sans CJK SC (SIL OFL)，TTC index 2，size 16。")
    lines.append(" * 收录源码中出现的 %d 个非 ASCII 字符；每字形 32 字节（16 行 x 16 位）。" % len(cps))
    lines.append(" */")
    lines.append("#ifndef AETHER_FONT_CJK_H")
    lines.append("#define AETHER_FONT_CJK_H")
    lines.append("")
    lines.append("#define AETHER_CJK_W 16u")
    lines.append("#define AETHER_CJK_H 16u")
    lines.append("#define AETHER_CJK_COUNT %d" % len(cps))
    lines.append("")
    lines.append("static const unsigned short aether_cjk_cp[AETHER_CJK_COUNT] = {")
    for i in range(0, len(cps), 12):
        lines.append("    " + ", ".join("0x%04X" % cp for cp in cps[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const unsigned char aether_cjk_glyph[AETHER_CJK_COUNT][32] = {")
    for i, g in enumerate(glyphs):
        lines.append("    { " + ", ".join("0x%02X" % b for b in g) + " },  /* U+%04X */" % cps[i])
    lines.append("};")
    lines.append("")
    lines.append("static inline int aether_cjk_find(unsigned cp)")
    lines.append("{")
    lines.append("    int lo = 0, hi = (int)AETHER_CJK_COUNT - 1;")
    lines.append("    while (lo <= hi) {")
    lines.append("        int mid = (lo + hi) / 2;")
    lines.append("        unsigned v = aether_cjk_cp[mid];")
    lines.append("        if (v == cp) return mid;")
    lines.append("        if (v < cp) lo = mid + 1; else hi = mid - 1;")
    lines.append("    }")
    lines.append("    return -1;")
    lines.append("}")
    lines.append("")
    lines.append("#endif /* AETHER_FONT_CJK_H */")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("生成:", os.path.abspath(OUT))
    print("收录字符:", len(cps), " 字库字节:", len(cps) * 32, " 文件字节:", os.path.getsize(OUT))
    # 抽查几个字形非空
    for probe in "中文件终端以太操作系统":
        i = cps.index(ord(probe)) if ord(probe) in cps else -1
        if i >= 0:
            print("  %s U+%04X 非空=%s" % (probe, ord(probe), any(glyphs[i])))


if __name__ == "__main__":
    main()
