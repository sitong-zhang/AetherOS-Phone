#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_icons.py - AetherOS 开源图标资产生成器

总纲第六章「图标铁律」：图标一律来自开源图标库，禁止自绘/手绘/AI 生成/
截图抠图；全系统只允许一套描边风格；必须成套、覆盖应用图标/状态栏符号/
设置项/控制中心磁贴/播放控制/通知小图标。

本脚本把 Lucide (ISC License, lucide-static v0.468.0) 的 24x24 统一描边
矢量图标，离线光栅化为 48x48 单通道 alpha 掩膜，编码为 C 数组头文件，随
源码分发。运行时按 alpha 以任意前景色合成、可整体缩放，不改动矢量来源。

用法:
    python3 tools/gen_icons.py <svg_dir> <output_header>

依赖: cairosvg, Pillow
生成物: aether-icons.h （纯数据 + 枚举 + 查表，可被 C 直接 include）
"""
import os
import sys
import glob
import re

ICON_W = 48
ICON_H = 48

# 系统语义名 -> Lucide 文件名。缺省时同名。用于生成稳定枚举与注释。
SEMANTIC = {
    "settings": "系统设置",
    "folder": "文件管理器",
    "terminal": "终端",
    "file-text": "文本编辑器",
    "calculator": "计算器",
    "activity": "系统监视器",
    "info": "关于本机",
    "layout-grid": "系统模块清单",
    "search": "搜索",
    "bell": "通知",
    "wifi": "Wi-Fi",
    "bluetooth": "蓝牙",
    "flashlight": "手电筒",
    "sun": "浅色主题",
    "moon": "深色主题",
    "volume-2": "音量",
    "battery-full": "电量",
    "battery-low": "低电量",
    "battery-charging": "充电",
    "signal": "移动信号",
    "home": "主页",
    "lock": "锁屏",
    "unlock": "解锁",
    "clock": "时钟",
    "calendar": "日历",
    "image": "图片",
    "music": "音乐",
    "play": "播放",
    "video": "视频",
    "globe": "浏览器",
    "download": "下载",
    "shield": "安全",
    "user": "账户",
    "users": "多用户",
    "power": "电源",
    "monitor": "显示",
    "cpu": "处理器",
    "hard-drive": "存储",
    "camera": "相机",
    "mic": "麦克风",
    "map-pin": "定位",
    "sliders": "控制中心",
    "wrench": "开发者选项",
    "palette": "主题",
    "type": "字体",
    "droplet": "色彩",
    "screenshot": "截屏",
    "timer": "计时器",
    "navigation": "导航",
    "phone": "通话",
    "message-circle": "消息",
}


def ident(name):
    """文件名 -> C 标识符（大写、下划线）"""
    s = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_").upper()
    if s and s[0].isdigit():
        s = "I_" + s
    return "AE_ICON_" + s


def render_svg(path):
    import cairosvg
    from PIL import Image
    import io
    with open(path, "rb") as f:
        svg = f.read()
    # currentColor -> 纯白；fill=none 保持；描边宽度/圆端点由源文件统一给出
    svg = svg.replace(b"currentColor", b"#ffffff")
    png = cairosvg.svg2png(bytestring=svg, output_width=ICON_W,
                           output_height=ICON_H)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    # 取 alpha 通道作为线性符号掩膜（0..255），前景色在运行时决定
    return list(im.getchannel("A").getdata())


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    svg_dir, out_path = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(os.path.join(svg_dir, "*.svg")))
    if not files:
        print("错误：未找到 SVG 文件: " + svg_dir)
        sys.exit(1)

    icons = []
    for p in files:
        name = os.path.splitext(os.path.basename(p))[0]
        alpha = render_svg(p)
        icons.append((name, ident(name), alpha))

    seen = {}
    for name, _, _ in icons:
        seen[name] = True

    # ---------- 轻量声明头：可被任意源文件 include，不含大块数据 ----------
    h = []
    h.append("/*")
    h.append(" * aether-icons.h - AetherOS 开源图标 API（自动生成，请勿手工编辑）")
    h.append(" *")
    h.append(" * 来源: Lucide (lucide-static v0.468.0), ISC License")
    h.append(" *       https://github.com/lucide-icons/lucide")
    h.append(" * 数据: aether-icons-data.h（仅由 aether-icons.c include 一次）")
    h.append(" * 图标数量: %d，全部同一开源库、同一描边体系。" % len(icons))
    h.append(" */")
    h.append("#ifndef AETHER_ICONS_H")
    h.append("#define AETHER_ICONS_H")
    h.append("")
    h.append("#define AE_ICON_W %d" % ICON_W)
    h.append("#define AE_ICON_H %d" % ICON_H)
    h.append("")
    h.append("enum aether_icon_id {")
    for i, (name, cid, _) in enumerate(icons):
        comma = "," if i < len(icons) - 1 else ""
        h.append("    %s = %d%s" % (cid, i, comma))
    h.append("};")
    h.append("#define AE_ICON_COUNT %d" % len(icons))
    h.append("")
    h.append("/* 把 id 对应的线性符号以 fg 前景色绘制到 (x,y) 的 size x size 区域。")
    h.append(" * 采用双线性采样缩放，放大后描边平滑；alpha 掩膜来自开源矢量图标。 */")
    h.append("void aether_draw_icon(int id, int x, int y, int size, unsigned fg);")
    h.append("")
    h.append("/* 应用图标磁贴：超椭圆玻璃底 + 上层单色线性符号 + 可选名称标签。")
    h.append(" * 底板/边框/高光由实现统一施加，保证同屏视觉重量一致。 */")
    h.append("void aether_draw_icon_tile(int id, int x, int y, int size,")
    h.append("                           unsigned glass, unsigned glyph,")
    h.append("                           const char *label, unsigned label_color);")
    h.append("")
    h.append("/* 图标名（诊断/无障碍语义） */")
    h.append("const char *aether_icon_name(int id);")
    h.append("")
    h.append("#endif /* AETHER_ICONS_H */")
    h.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(h))

    # ---------- 数据头：仅 aether-icons.c include 一次 ----------
    data_path = out_path[:-2] + "-data.h" if out_path.endswith(".h") else out_path + "-data.h"
    d = []
    d.append("/* aether-icons-data.h - 自动生成：Lucide (ISC) alpha 掩膜数据，勿编辑 */")
    d.append("#ifndef AETHER_ICONS_DATA_H")
    d.append("#define AETHER_ICONS_DATA_H")
    d.append('#include "aether-icons.h"')
    d.append("")
    for name, cid, alpha in icons:
        cn = SEMANTIC.get(name, "")
        d.append("/* %s%s */" % (name, (" - " + cn) if cn else ""))
        d.append("static const unsigned char %s_data[AE_ICON_W * AE_ICON_H] = {" % cid.lower())
        for y in range(ICON_H):
            row = alpha[y * ICON_W:(y + 1) * ICON_W]
            d.append("    " + ",".join("%3d" % v for v in row) + ",")
        d.append("};")
        d.append("")
    d.append("static const unsigned char *const ae_icon_bits[AE_ICON_COUNT] = {")
    for _, cid, _ in icons:
        d.append("    %s_data," % cid.lower())
    d.append("};")
    d.append("")
    d.append("static const char *const ae_icon_names[AE_ICON_COUNT] = {")
    for name, _, _ in icons:
        d.append('    "%s",' % name)
    d.append("};")
    d.append("")
    d.append("#endif /* AETHER_ICONS_DATA_H */")
    d.append("")

    with open(data_path, "w", encoding="utf-8") as f:
        f.write("\n".join(d))

    print("已生成 %s (%d 字节) 与 %s (%d 字节)：%d 个图标" %
          (out_path, os.path.getsize(out_path), data_path,
           os.path.getsize(data_path), len(icons)))
    print("图标清单: " + ", ".join(n for n, _, _ in icons))


if __name__ == "__main__":
    main()
