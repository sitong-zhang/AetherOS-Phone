#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_vector_font.py — 生成 AetherOS 客机矢量字体 (TrueType / glyf 轮廓)

来源与许可
  - 字形来源: Noto Sans CJK SC (TTC index 2), SIL Open Font License 1.1
    系统路径 /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
  - 轮廓转换: fontTools (Cu2QuPen 三次贝塞尔降二次 + TTGlyphPen 生成 TrueType glyf)
    原字体为 CFF (PostScript 三次曲线) 轮廓，stb_truetype 仅支持 glyf (二次曲线)，
    故离线转换为 TrueType 并做子集，体积由 ~16 MB 降至 ~2 MB。
  - 栅格化: stb_truetype.h v1.08 (public domain), 静态链入 aether-compositor。
  - 收录范围: ASCII 0x20-0x7E + GB2312 全量 6763 汉字与符号 + 常用标点/箭头/
    全角字符 + 客机源码中出现过的全部非 ASCII 字符，覆盖 7700+ 码点。

用途
  替换旧 8x16 ASCII / 16x16 CJK 点阵: 覆盖全 CJK、任意尺寸平滑缩放、抗锯齿；
  TTF 由构建脚本经 objcopy 转为目标文件静态链入合成器，无运行时文件依赖。

OFL 合规
  本字体是 Noto Sans CJK SC 的修改版 (子集 + 轮廓格式转换 + 曲线降阶)，
  按 SIL OFL 1.1 第 3 条改用了独立的字体名 "Aether Sans CJK"，不使用保留字体名；
  随附 OFL 全文见 assets/OFL.txt，归属见 OPENSOURCE-ATTRIBUTION.md。

输出: assets/aether-font.ttf
"""
import glob
import os
import sys

from fontTools.ttLib import TTCollection, TTFont, newTable
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools import subset

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_TTC = os.environ.get(
    "AETHER_FONT_SRC",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
)
TTC_INDEX = 2  # Noto Sans CJK SC
OUT = os.path.join(ROOT, "assets", "aether-font.ttf")


def collect_codepoints():
    """ASCII + GB2312 全量 + 常用符号 + 源码实字符。"""
    cps = set(range(0x20, 0x7F))
    # GB2312 双字节区 (含 6763 汉字与全角标点/符号)
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                cps.add(ord(bytes([hi, lo]).decode("gb2312")))
            except (UnicodeDecodeError, ValueError):
                pass
    for c in (
        "°·π—…→←↑↓★☆✓✔✕✖●○◆■□▲▼∞≈≠≤≥±×÷℃￥§¶†‡"
        "「」『』【】《》〔〕、。，！？：；～－"
    ):
        cps.add(ord(c))
    cps |= set(range(0x3000, 0x3040)) | set(range(0xFF00, 0xFFF0))
    # 客机源码里出现过的所有非 ASCII 字符（UI 文案、状态串）
    for p in glob.glob(os.path.join(ROOT, "src", "*.[ch]")):
        try:
            s = open(p, encoding="utf-8").read()
        except (OSError, UnicodeDecodeError):
            continue
        for ch in s:
            if ord(ch) >= 0x80:
                cps.add(ord(ch))
    return cps


def rename_font(f):
    """OFL 第 3 条：修改版不得使用保留字体名。"""
    if "name" not in f:
        return
    for rec in f["name"].names:
        try:
            s = rec.toUnicode()
        except Exception:
            continue
        if ("Noto Sans CJK" not in s) and ("NotoSansCJK" not in s):
            continue
        s2 = (
            s.replace("Noto Sans CJK SC", "Aether Sans CJK SC")
            .replace("Noto Sans CJK", "Aether Sans CJK")
            .replace("NotoSansCJK", "AetherSansCJK")
        )
        try:
            rec.string = s2.encode(rec.getEncoding())
        except Exception:
            pass


def main():
    if not os.path.exists(SRC_TTC):
        print("缺少源字体: %s" % SRC_TTC, file=sys.stderr)
        return 2
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    cps = collect_codepoints()
    print("请求码点 %d 个" % len(cps), flush=True)

    coll = TTCollection(SRC_TTC, lazy=False)
    f = coll.fonts[TTC_INDEX]
    print("原始字形 %d" % len(f.getGlyphOrder()), flush=True)

    opt = subset.Options()
    opt.glyph_names = False
    opt.notdef_outline = True
    opt.recalc_bounds = True
    opt.layout_features = []
    opt.name_IDs = ["*"]
    opt.name_legacy = True
    opt.drop_tables += ["DSIG", "VORG", "BASE", "JSTF", "MATH"]
    sub = subset.Subsetter(options=opt)
    sub.populate(unicodes=cps)
    sub.subset(f)
    print("子集后字形 %d" % len(f.getGlyphOrder()), flush=True)

    # CFF (三次) -> glyf (二次): Cu2QuPen 降阶，TTGlyphPen 生成 TrueType 轮廓
    gs = f.getGlyphSet()
    order = f.getGlyphOrder()
    glyf = newTable("glyf")
    glyf.glyphOrder = order
    glyf.glyphs = {}
    for name in order:
        pen = TTGlyphPen(None)
        gs[name].draw(Cu2QuPen(pen, max_err=1.0, reverse_direction=True))
        glyf[name] = pen.glyph()
    f["glyf"] = glyf
    f["loca"] = newTable("loca")
    for tag in ("CFF ", "CFF2", "VORG"):
        if tag in f:
            del f[tag]

    mp = newTable("maxp")
    mp.tableVersion = 0x00010000
    mp.numGlyphs = len(order)
    # maxp 1.0 附加字段：先给全，避免 sstruct.pack 缺字段
    mp.maxZones = 1
    mp.maxTwilightPoints = 0
    mp.maxStorage = 0
    mp.maxFunctionDefs = 0
    mp.maxInstructionDefs = 0
    mp.maxStackElements = 0
    mp.maxSizeOfInstructions = 0
    mp.maxComponentElements = 0
    mp.maxComponentDepth = 0
    f["maxp"] = mp
    for nm in order:
        glyf[nm].recalcBounds(glyf)
    f["maxp"].recalc(f)
    f["head"].glyphDataFormat = 0
    f["post"].formatType = 3.0
    rename_font(f)
    f.sfntVersion = "\x00\x01\x00\x00"
    f.save(OUT, reorderTables=True)
    print("输出 %s  %d 字节" % (OUT, os.path.getsize(OUT)), flush=True)

    # 复核
    g = TTFont(OUT)
    cm = set()
    for t in g["cmap"].tables:
        cm |= set(t.cmap.keys())
    cjk = sum(1 for c in cm if 0x4E00 <= c <= 0x9FFF)
    print(
        "复核: glyf=%s 码点=%d CJK=%d cmap=%s"
        % ("glyf" in g, len(cm), cjk,
           [(t.platformID, t.platEncID, t.format) for t in g["cmap"].tables])
    )
    miss = [c for c in "笔系虚拟桌面应用商店笔记更新矢量字体" if ord(c) not in cm]
    print("关键字覆盖: %s" % ("全部命中" if not miss else "缺 " + "".join(miss)))
    return 0 if "glyf" in g and not miss else 1


if __name__ == "__main__":
    sys.exit(main())
