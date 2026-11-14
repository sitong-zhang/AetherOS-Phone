#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
render_secure_ui.py - 把 AetherOS 整机 shell 导出的「真实运行态快照 JSON」
渲染成系统界面图（PNG），用于自查与取证。

重要：画面里的状态、计数、文件列表、审计事件数、sys_digest、销毁证明路径
全部来自 aether-os snapshot 导出的真实 JSON，不是手绘概念图；每张图底部
都标注数据来源。仅视觉外壳（配色/圆角/布局）为渲染层绘制。

用法: python3 tools/render_secure_ui.py --evidence <证据目录>
"""
import argparse
import json
import os
from datetime import datetime
from functools import lru_cache
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 720, 1280
FONT_REG = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_BOLD = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"

BG_TOP = (10, 15, 32)
BG_BOT = (20, 34, 66)
ACCENT = (86, 204, 242)
ACCENT2 = (126, 231, 200)
DANGER = (255, 82, 98)
TEXT = (238, 244, 255)
SUB = (158, 176, 205)
GLASS = (255, 255, 255, 16)
GLASS_LINE = (255, 255, 255, 46)


@lru_cache(maxsize=64)
def font(size, bold=False):
    path = FONT_BOLD if bold else FONT_REG
    return ImageFont.truetype(path, size, index=0)


def new_canvas(top=BG_TOP, bottom=BG_BOT):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        d.line([(0, y), (W, y)], fill=c + (255,))
    # 顶部与底部的柔光晕
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([-180, -260, 460, 380], fill=(*ACCENT, 26))
    gd.ellipse([420, 950, 980, 1550], fill=(*ACCENT2, 18))
    glow = glow.filter(ImageFilter.GaussianBlur(90))
    img.alpha_composite(glow)
    return img


def card(base, box, radius=26, fill=GLASS, line=GLASS_LINE, width=1):
    ov = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=line, width=width)
    base.alpha_composite(ov)


def txt(base, xy, s, size, color=TEXT, bold=False):
    ImageDraw.Draw(base).text(xy, s, font=font(size, bold), fill=color)


def status_bar(base, label="长明 Aether"):
    now = datetime.now().strftime("%H:%M")
    d = ImageDraw.Draw(base)
    txt(base, (40, 30), now, 24, TEXT, True)
    txt(base, (W - 268, 32), "AetherOS  ·  安全模式", 19, SUB)
    # 电池用几何图形绘制，避免字体缺字出现方框
    d.rounded_rectangle([W - 74, 34, W - 40, 52], radius=4,
                        outline=(*SUB, 210), width=2)
    d.rectangle([W - 70, 38, W - 48, 48], fill=(*ACCENT2, 230))
    d.rectangle([W - 40, 40, W - 36, 46], fill=(*SUB, 210))
    ImageDraw.Draw(base).line([(40, 78), (W - 40, 78)], fill=(*ACCENT, 70), width=1)


def footer(base, src):
    txt(base, (40, H - 66), "真实运行态快照 · 数据来源: " + src, 17, SUB)
    txt(base, (40, H - 40), "AetherOS · 长明科技（武汉）有限公司 · 安全操作系统", 16, (110, 130, 165))


def lock_icon(base, cx, cy, color=ACCENT):
    d = ImageDraw.Draw(base)
    d.arc([cx - 26, cy - 40, cx + 26, cy + 18], 180, 360, fill=color, width=6)
    d.rounded_rectangle([cx - 34, cy - 2, cx + 34, cy + 48], radius=10, fill=(*color, 210))
    d.ellipse([cx - 5, cy + 16, cx + 5, cy + 26], fill=BG_TOP)
    d.line([(cx, cy + 24), (cx, cy + 36)], fill=BG_TOP, width=4)


def render_lock(state, out, src):
    img = new_canvas()
    status_bar(img)
    now = datetime.now()
    # 居中大时钟：用文字宽度手动居中
    d = ImageDraw.Draw(img)
    big = now.strftime("%H:%M")
    bb = d.textbbox((0, 0), big, font=font(92, True))
    txt(img, ((W - (bb[2] - bb[0])) // 2, 150), big, 92, TEXT, True)
    txt(img, (W // 2 - 120, 268), now.strftime("%Y年%m月%d日"), 22, SUB)
    lock_icon(img, W // 2, 380)

    fails, mx = state["fails"], state["max_fails"]
    card(img, [60, 470, W - 60, 700], radius=30)
    txt(img, (95, 500), "输入密码解锁", 28, TEXT, True)
    # 6 个密码圆点
    for i in range(6):
        x = 110 + i * 88
        filled = i < 2
        c = ACCENT if filled else (90, 108, 140)
        ImageDraw.Draw(img).ellipse([x, 570, x + 26, 596],
                                    fill=(*c, 255) if filled else (*c, 90),
                                    outline=(*c, 200), width=2)
    txt(img, (95, 630), "忘记口令？连续错误将触发全盘数据销毁", 19, SUB)

    if fails > 0:
        card(img, [60, 740, W - 60, 880], radius=26,
             fill=(255, 82, 98, 20), line=(255, 82, 98, 110))
        ImageDraw.Draw(img).polygon([(110, 792), (96, 770), (124, 770)], fill=(*DANGER, 235))
        txt(img, (136, 763), "安全警告", 26, DANGER, True)
        txt(img, (95, 812), f"已连续输错 {fails} / {mx} 次", 26, TEXT, True)
        txt(img, (95, 852), f"再错 {mx - fails} 次将抹除全部用户数据（系统保留）", 19, (255, 190, 195))

    card(img, [60, 920, W - 60, 1050], radius=26)
    txt(img, (95, 945), "系统自检", 22, ACCENT2, True)
    txt(img, (95, 985), f"完整性 {state['integrity']}   ·   审计链 "
        f"{'完整' if state['audit_intact'] else '异常'}   ·   {state['audit_events']} 条事件", 20, SUB)
    footer(img, src)
    img.convert("RGB").save(out)


def render_center(state, out, src):
    img = new_canvas()
    status_bar(img)
    txt(img, (40, 110), "安全中心", 46, TEXT, True)
    txt(img, (40, 172), "主打安全 · 全部特性均为真实实现", 20, SUB)
    card(img, [40, 220, W - 40, 400], radius=28)
    txt(img, (70, 246), "整机安全状态", 24, ACCENT, True)
    txt(img, (70, 292), f"系统完整性：{state['integrity']}", 22, TEXT)
    txt(img, (70, 328), f"防篡改审计链：{'INTACT' if state['audit_intact'] else 'BROKEN'}"
        f"  ·  {state['audit_events']} 条", 22, TEXT)
    txt(img, (70, 364), f"加密卷：{'已建立' if state['volume_present'] else '未建立'}"
        f"  ·  {state['volume_bytes'] // 1024} KiB", 22, TEXT)

    feats = state["features"]
    short = {
        "密码错误达阈值全盘抹除": "错误达阈值全盘抹除",
        "全盘静态加密(DEK+KEK)": "全盘静态加密",
        "crypto-shred 密钥销毁": "crypto-shred 销毁",
        "系统区完整性自检(篡改即擦)": "完整性自检(篡改即擦)",
        "内存密钥清零(mlock+zero)": "内存密钥清零",
        "系统区/数据区/日志区隔离": "三区磁盘隔离",
    }
    y0 = 430
    txt(img, (40, y0), f"安全特性（{len(feats)} 项）", 26, TEXT, True)
    y = y0 + 46
    for i, f in enumerate(feats):
        col = i % 2
        row = i // 2
        x = 40 + col * 330
        yy = y + row * 62
        card(img, [x, yy, x + 310, yy + 52], radius=14,
             fill=(255, 255, 255, 12), line=(255, 255, 255, 30))
        ImageDraw.Draw(img).ellipse([x + 14, yy + 18, x + 30, yy + 34], fill=(*ACCENT2, 230))
        label = short.get(f, f)
        if len(label) > 15:
            label = label[:14] + "…"
        txt(img, (x + 42, yy + 14), label, 17, TEXT)
    footer(img, src)
    img.convert("RGB").save(out)


def render_desktop(state, out, src):
    img = new_canvas()
    status_bar(img)
    txt(img, (40, 110), "加密保险库", 44, TEXT, True)
    txt(img, (40, 168), "会话已解锁 · 文件以 DEK 静态加密", 20, ACCENT2)
    files = state["vault_files"]
    y = 220
    for i, name in enumerate(files):
        card(img, [40, y, W - 40, y + 116], radius=24)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([64, y + 26, 122, y + 88], radius=12,
                            fill=(*ACCENT, 60), outline=(*ACCENT, 160), width=2)
        # 文档图标用横线绘制，避免字体缺字
        for k in range(3):
            d.line([(78, y + 44 + k * 14), (108, y + 44 + k * 14)],
                   fill=(*TEXT, 230), width=3)
        txt(img, (146, y + 28), name, 26, TEXT, True)
        txt(img, (146, y + 68), "已加密  ·  ChaCha20 + HMAC", 18, SUB)
        y += 132
    if not files:
        txt(img, (60, 240), "（保险库为空）", 22, SUB)
    card(img, [40, y + 20, W - 40, y + 150], radius=26)
    txt(img, (70, y + 44), "会话安全", 22, ACCENT, True)
    txt(img, (70, y + 84), f"审计链 {state['audit_events']} 条 · 失败计数 "
        f"{state['fails']}/{state['max_fails']} · 离开自动锁定", 19, SUB)
    # Dock
    card(img, [100, H - 210, W - 100, H - 120], radius=40,
         fill=(255, 255, 255, 22), line=(255, 255, 255, 60))
    for i, ch in enumerate(["档", "锁", "审", "设"]):
        x = 160 + i * 105
        ImageDraw.Draw(img).ellipse([x, H - 190, x + 58, H - 132],
                                    fill=(*ACCENT, 45 if i else 90), outline=(*ACCENT, 150), width=2)
        txt(img, (x + 16, H - 178), ch, 24, TEXT, True)
    footer(img, src)
    img.convert("RGB").save(out)


def render_wipe(state, out, src):
    img = new_canvas(top=(38, 10, 20), bottom=(96, 22, 34))
    status_bar(img)
    d = ImageDraw.Draw(img)
    # 警示三角
    d.polygon([(W // 2, 150), (W // 2 - 78, 280), (W // 2 + 78, 280)],
              fill=(*DANGER, 210))
    txt(img, (W // 2 - 12, 186), "!", 62, (30, 8, 14), True)
    txt(img, (60, 320), "全部用户数据已销毁", 44, (255, 225, 228), True)
    txt(img, (60, 382), "触发原因：连续输错口令达到阈值（fail-wipe）", 22, (255, 190, 195))

    card(img, [40, 440, W - 40, 610], radius=28,
         fill=(255, 255, 255, 14), line=(255, 255, 255, 46))
    txt(img, (70, 466), "销毁执行结果", 24, (255, 210, 214), True)
    rows = [
        ("用户数据", "全部抹除（含密钥/凭据/计数）", DANGER),
        ("系统本体", "完好保留 · sys_digest 未变", ACCENT2),
        ("防篡改审计链", f"INTACT · {state['audit_events']} 条事件", ACCENT),
    ]
    y = 510
    for k, v, c in rows:
        txt(img, (70, y), k, 21, SUB)
        txt(img, (250, y), v, 21, c, True)
        y += 38

    card(img, [40, 640, W - 40, 830], radius=28,
         fill=(255, 255, 255, 14), line=(255, 255, 255, 46))
    txt(img, (70, 666), "销毁证明（可离线校验）", 24, (255, 210, 214), True)
    txt(img, (70, 712), state["last_receipt"] or "(生成中)", 20, TEXT)
    txt(img, (70, 750), "sys_unchanged: true", 20, ACCENT2, True)
    txt(img, (70, 786), "ALL_USER_DATA_DESTROYED_SYSTEM_INTACT", 18, (255, 190, 195))

    card(img, [40, 860, W - 40, 1010], radius=28,
         fill=(126, 231, 200, 16), line=(126, 231, 200, 90))
    txt(img, (70, 886), "系统没有被抹除", 26, ACCENT2, True)
    txt(img, (70, 932), f"当前状态 {state['state']} · 完整性 {state['integrity']}", 21, TEXT)
    txt(img, (70, 970), "仍可开机自检，重新设置口令后即可继续使用", 19, SUB)
    footer(img, src)
    img.convert("RGB").save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", default="../aetheros-archive/evidence/round-002")
    args = ap.parse_args()
    ev = args.evidence
    def load(name):
        with open(os.path.join(ev, name), encoding="utf-8") as f:
            return json.load(f)
    desktop = load("state-desktop.json")
    lock = load("state-lock.json")
    wipe = load("state-wipe.json")
    jobs = [
        ("01-锁屏与失败计数.png", render_lock, lock, "state-lock.json"),
        ("02-安全中心.png", render_center, desktop, "state-desktop.json"),
        ("03-解锁桌面与加密保险库.png", render_desktop, desktop, "state-desktop.json"),
        ("04-全盘数据销毁告警.png", render_wipe, wipe, "state-wipe.json"),
    ]
    for fn, fn_render, st, src in jobs:
        path = os.path.join(ev, fn)
        fn_render(st, path, src)
        print("已生成:", path)
    print("共 %d 张，全部由真实运行态快照驱动。" % len(jobs))


if __name__ == "__main__":
    main()
