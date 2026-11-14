#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
render_secure_ui.py - 由整机 shell 的运行态快照渲染界面图。

画面只呈现产品界面本身：状态、计数与文件列表来自快照，但不展示内部
技术标识、取证标注、执行证明或机制说明。自毁触发以动效帧序列呈现，
不使用结果报告式页面。

用法: python3 tools/render_secure_ui.py --evidence <证据目录>
"""
import argparse
import json
import os
import random
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


def status_bar(base):
    now = datetime.now().strftime("%H:%M")
    d = ImageDraw.Draw(base)
    txt(base, (40, 30), now, 24, TEXT, True)
    txt(base, (W - 268, 32), "AetherOS  ·  安全模式", 19, SUB)
    d.rounded_rectangle([W - 74, 34, W - 40, 52], radius=4,
                        outline=(*SUB, 210), width=2)
    d.rectangle([W - 70, 38, W - 48, 48], fill=(*ACCENT2, 230))
    d.rectangle([W - 40, 40, W - 36, 46], fill=(*SUB, 210))
    d.line([(40, 78), (W - 40, 78)], fill=(*ACCENT, 70), width=1)


def lock_icon(base, cx, cy, color=ACCENT):
    d = ImageDraw.Draw(base)
    d.arc([cx - 26, cy - 40, cx + 26, cy + 18], 180, 360, fill=color, width=6)
    d.rounded_rectangle([cx - 34, cy - 2, cx + 34, cy + 48], radius=10, fill=(*color, 210))
    d.ellipse([cx - 5, cy + 16, cx + 5, cy + 26], fill=BG_TOP)
    d.line([(cx, cy + 24), (cx, cy + 36)], fill=BG_TOP, width=4)


def render_lock(state, out):
    img = new_canvas()
    status_bar(img)
    now = datetime.now()
    d = ImageDraw.Draw(img)
    big = now.strftime("%H:%M")
    bb = d.textbbox((0, 0), big, font=font(92, True))
    txt(img, ((W - (bb[2] - bb[0])) // 2, 150), big, 92, TEXT, True)
    txt(img, (W // 2 - 120, 268), now.strftime("%Y年%m月%d日"), 22, SUB)
    lock_icon(img, W // 2, 380)

    fails, mx = state["fails"], state["max_fails"]
    card(img, [60, 470, W - 60, 700], radius=30)
    txt(img, (95, 500), "输入密码解锁", 28, TEXT, True)
    for i in range(6):
        x = 110 + i * 88
        filled = i < 2
        c = ACCENT if filled else (90, 108, 140)
        ImageDraw.Draw(img).ellipse([x, 570, x + 26, 596],
                                    fill=(*c, 255) if filled else (*c, 90),
                                    outline=(*c, 200), width=2)
    txt(img, (95, 630), "连续输错将抹除全部数据", 19, SUB)

    if fails > 0:
        card(img, [60, 740, W - 60, 880], radius=26,
             fill=(255, 82, 98, 20), line=(255, 82, 98, 110))
        ImageDraw.Draw(img).polygon([(110, 792), (96, 770), (124, 770)], fill=(*DANGER, 235))
        txt(img, (136, 763), "安全警告", 26, DANGER, True)
        txt(img, (95, 812), f"已连续输错 {fails} / {mx} 次", 26, TEXT, True)
        txt(img, (95, 852), f"再错 {mx - fails} 次将抹除全部用户数据", 19, (255, 190, 195))
    img.convert("RGB").save(out)


def render_center(state, out):
    img = new_canvas()
    status_bar(img)
    txt(img, (40, 110), "安全中心", 46, TEXT, True)

    state_text = "正常" if state["integrity"] == "OK" else "异常"
    card(img, [40, 220, W - 40, 400], radius=28)
    txt(img, (70, 246), "整机安全状态", 24, ACCENT, True)
    txt(img, (70, 292), f"系统状态：{state_text}", 22, TEXT)
    txt(img, (70, 328), "安全记录：完整" if state["audit_intact"] else "安全记录：异常", 22, TEXT)
    txt(img, (70, 364), "加密存储：已启用" if state["volume_present"] else "加密存储：未启用", 22, TEXT)

    feats = state["features"]
    # 内部取证能力（销毁证明等）不进入面向用户的能力列表
    short = {
        "密码错误达阈值全盘抹除": "错误超限自动抹除",
        "胁迫口令": "胁迫口令",
        "防重启绕过计数": "防重启绕过",
        "指数退避": "登录退避",
        "全盘静态加密(DEK+KEK)": "全盘加密",
        "crypto-shred 密钥销毁": "密钥销毁",
        "多轮覆写数据卷": "数据覆写",
        "只追加防篡改审计链": "防篡改记录",
        "系统区完整性自检(篡改即擦)": "完整性保护",
        "会话空闲锁定": "空闲自动锁定",
        "内存密钥清零(mlock+zero)": "内存密钥保护",
        "紧急一键销毁": "紧急销毁",
        "系统区/数据区/日志区隔离": "分区隔离",
    }
    shown = [short[f] for f in feats if f in short]
    y0 = 430
    txt(img, (40, y0), f"安全能力（{len(shown)} 项）", 26, TEXT, True)
    y = y0 + 46
    for i, label in enumerate(shown):
        col = i % 2
        row = i // 2
        x = 40 + col * 330
        yy = y + row * 62
        card(img, [x, yy, x + 310, yy + 52], radius=14,
             fill=(255, 255, 255, 12), line=(255, 255, 255, 30))
        ImageDraw.Draw(img).ellipse([x + 14, yy + 18, x + 30, yy + 34], fill=(*ACCENT2, 230))
        if len(label) > 15:
            label = label[:14] + "…"
        txt(img, (x + 42, yy + 14), label, 17, TEXT)
    img.convert("RGB").save(out)


def render_desktop(state, out):
    img = new_canvas()
    status_bar(img)
    txt(img, (40, 110), "加密保险库", 44, TEXT, True)
    txt(img, (40, 168), "会话已解锁", 20, ACCENT2)
    files = state["vault_files"]
    y = 220
    for name in files:
        card(img, [40, y, W - 40, y + 116], radius=24)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([64, y + 26, 122, y + 88], radius=12,
                            fill=(*ACCENT, 60), outline=(*ACCENT, 160), width=2)
        for k in range(3):
            d.line([(78, y + 44 + k * 14), (108, y + 44 + k * 14)],
                   fill=(*TEXT, 230), width=3)
        txt(img, (146, y + 28), name, 26, TEXT, True)
        txt(img, (146, y + 68), "已加密", 18, SUB)
        y += 132
    if not files:
        txt(img, (60, 240), "保险库为空", 22, SUB)
    card(img, [40, y + 20, W - 40, y + 150], radius=26)
    txt(img, (70, y + 44), "会话安全", 22, ACCENT, True)
    txt(img, (70, y + 84), "离开后自动锁定", 19, SUB)
    card(img, [100, H - 210, W - 100, H - 120], radius=40,
         fill=(255, 255, 255, 22), line=(255, 255, 255, 60))
    for i, ch in enumerate(["档", "锁", "审", "设"]):
        x = 160 + i * 105
        ImageDraw.Draw(img).ellipse([x, H - 190, x + 58, H - 132],
                                    fill=(*ACCENT, 45 if i else 90), outline=(*ACCENT, 150), width=2)
        txt(img, (x + 16, H - 178), ch, 24, TEXT, True)
    img.convert("RGB").save(out)


def render_wipe_sequence(evdir):
    """自毁触发动效：警示 → 数据碎裂消散 → 熄灭定格。

    全程不出现触发原因、执行结果、证明路径、系统状态或任何技术字段。
    返回生成的帧数；定格帧沿用既有界面图文件名。
    """
    rng = random.Random(20261114)
    particles = []
    for _ in range(320):
        particles.append((rng.uniform(70, W - 70), rng.uniform(220, H - 160),
                          rng.uniform(1.5, 4.5), rng.uniform(0.35, 1.0)))

    outdir = os.path.join(evdir, "wipe-animation")
    os.makedirs(outdir, exist_ok=True)
    frames = 16
    last = None
    for i in range(frames):
        t = i / (frames - 1)
        fade = 1.0 - t
        top = (int(38 * fade + 6 * t), int(10 * fade + 6 * t), int(20 * fade + 12 * t))
        bot = (int(96 * fade + 10 * t), int(22 * fade + 12 * t), int(34 * fade + 20 * t))
        img = new_canvas(top=top, bottom=bot)
        d = ImageDraw.Draw(img)

        if t < 0.35:
            a = int(235 * min(1.0, t / 0.18))
            tri = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            td = ImageDraw.Draw(tri)
            td.polygon([(W // 2, 150), (W // 2 - 78, 280), (W // 2 + 78, 280)],
                       fill=(*DANGER, a))
            td.text((W // 2 - 12, 186), "!", font=font(62, True), fill=(30, 8, 14, a))
            img.alpha_composite(tri)
            sy = int(120 + (H - 200) * (t / 0.35))
            d.line([(40, sy), (W - 40, sy)], fill=(*DANGER, int(160 * fade)), width=2)

        if 0.12 <= t < 0.82:
            p = (t - 0.12) / 0.70
            ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            od = ImageDraw.Draw(ov)
            for (x, y, r, w) in particles:
                dx = (x - W / 2) * (0.35 + 1.5 * p)
                dy = (y - H / 2) * (0.35 + 1.5 * p)
                px, py = x + dx * p, y + dy * p
                alpha = int(220 * (1.0 - p) * w)
                if alpha > 0:
                    od.ellipse([px - r, py - r, px + r, py + r],
                               fill=(255, 190, 195, alpha))
            img.alpha_composite(ov)

        if t >= 0.78:
            q = min(1.0, (t - 0.78) / 0.22)
            lock_icon(img, W // 2, H // 2 - 40, color=ACCENT)
            ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            od = ImageDraw.Draw(ov)
            od.text((W // 2 - 108, H // 2 + 60), "设备已锁定",
                    font=font(34, True), fill=(*TEXT, int(255 * q)))
            img.alpha_composite(ov)

        last = img
        img.convert("RGB").save(os.path.join(outdir, f"frame-{i:02d}.png"))

    last.convert("RGB").save(os.path.join(evdir, "04-全盘数据销毁告警.png"))
    return frames


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
    jobs = [
        ("01-锁屏与失败计数.png", render_lock, lock),
        ("02-安全中心.png", render_center, desktop),
        ("03-解锁桌面与加密保险库.png", render_desktop, desktop),
    ]
    for fn, fn_render, st in jobs:
        path = os.path.join(ev, fn)
        fn_render(st, path)
        print("已生成:", path)
    n = render_wipe_sequence(ev)
    print("已生成: 自毁动效 %d 帧 + 定格图 04-全盘数据销毁告警.png" % n)


if __name__ == "__main__":
    main()
