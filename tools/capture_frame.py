#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""capture_frame.py - 通过 QEMU QMP screendump 抓取真实图形首帧并做像素级断言。

与「自检打印 ALL PASSED」不同，本工具从 QEMU 侧读取 virtio-gpu 实际 scanout 的
framebuffer（PPM），转换为 PNG，并统计颜色/非背景像素占比，作为外部可观测证据：
  - 画面尺寸、颜色种类
  - 非背景像素占比（有内容而非全黑/纯色）
  - 连续两帧是否稳定（间隔采样，确认合成器持续上屏而非一闪而过）

用法：
  capture_frame.py --qmp <unix-socket> --out <png> [--ppm <ppm>] [--json <json>] [--frames N]
退出码：0=抓帧成功且判定为有效图形帧；非 0=失败（供门禁断言）。
"""
import argparse
import json
import os
import socket
import sys
import time

try:
    from PIL import Image
except ImportError:
    print("需要 Pillow：pip3 install Pillow", file=sys.stderr)
    sys.exit(2)


class QMP:
    def __init__(self, path, timeout=10):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.f = self.s.makefile("rwb")
        self.f.readline()  # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, execute, arguments=None):
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        # 丢弃异步事件，直到拿到 return/error
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP 连接关闭")
            obj = json.loads(line.decode())
            if "return" in obj or "error" in obj:
                return obj

    def screendump(self, ppm_path):
        return self.cmd("screendump", {"filename": ppm_path})

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def analyze(ppm_path):
    """读取 P6 PPM，返回像素统计。"""
    im = Image.open(ppm_path).convert("RGB")
    w, h = im.size
    # 下采样统计，兼顾速度；用 tobytes 逐像素，避免已弃用的 getdata。
    small = im.resize((min(w, 320), min(h, 200)))
    raw = small.tobytes()  # RGBRGB...
    total = len(raw) // 3
    non_bg = 0
    colors = set()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        if max(r, g, b) > 24:
            non_bg += 1
        colors.add((r, g, b))
    return {
        "width": w,
        "height": h,
        "sample_colors": len(colors),
        "non_bg_ratio": round(non_bg / total, 4) if total else 0.0,
        "non_bg_pixels": non_bg,
        "sample_pixels": total,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True, help="QEMU QMP unix socket 路径")
    ap.add_argument("--out", required=True, help="输出 PNG 路径")
    ap.add_argument("--ppm", default="", help="中间 PPM 路径（默认 <out>.ppm）")
    ap.add_argument("--json", default="", help="输出统计 JSON 路径")
    ap.add_argument("--frames", type=int, default=2, help="连续采样帧数")
    ap.add_argument("--interval", type=float, default=1.0, help="帧间隔秒")
    ap.add_argument("--min-colors", type=int, default=16, help="有效帧最少颜色种类")
    ap.add_argument("--min-non-bg", type=float, default=0.02, help="有效帧最少非背景占比")
    args = ap.parse_args()

    ppm = args.ppm or (args.out + ".ppm")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    q = QMP(args.qmp)
    stats = []
    final_ppm = ""
    try:
        for i in range(args.frames):
            p = ppm if args.frames == 1 else (ppm + ("" if i == args.frames - 1 else ".%d" % i))
            resp = q.screendump(p)
            if "error" in resp:
                print("screendump 失败：", resp["error"], file=sys.stderr)
                return 3
            time.sleep(0.2)
            if not os.path.exists(p):
                print("QMP 未产出 PPM：", p, file=sys.stderr)
                return 3
            st = analyze(p)
            st["frame"] = i
            stats.append(st)
            final_ppm = p
            print("[capture] 第 %d 帧 %dx%d 颜色=%d 非背景=%.3f" %
                  (i, st["width"], st["height"], st["sample_colors"], st["non_bg_ratio"]))
            if i < args.frames - 1:
                time.sleep(args.interval)
    finally:
        q.close()

    # 以最后一帧作为交付 PNG
    Image.open(final_ppm).convert("RGB").save(args.out, "PNG")

    last = stats[-1]
    valid = (last["sample_colors"] >= args.min_colors
             and last["non_bg_ratio"] >= args.min_non_bg)
    # 连续帧稳定：各帧都达到内容阈值（时钟/动画会变化，但不应出现全黑帧）
    stable = all(s["sample_colors"] >= args.min_colors
                 and s["non_bg_ratio"] >= args.min_non_bg for s in stats)

    result = {
        "valid": bool(valid and stable),
        "stable": bool(stable),
        "frames": len(stats),
        "min_colors": args.min_colors,
        "min_non_bg": args.min_non_bg,
        "samples": stats,
        "png": os.path.abspath(args.out),
        "source": "QEMU QMP screendump（virtio-gpu 真实 scanout）",
    }
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)

    print("[capture] 判定：%s（valid=%s stable=%s）" %
          ("有效真实首帧" if result["valid"] else "无效/空白帧", valid, stable))
    return 0 if result["valid"] else 4


if __name__ == "__main__":
    sys.exit(main())
