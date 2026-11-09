#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_assets.py - 生成 AetherOS 系统内置真实资产（总纲 6.6「壁纸成套且风格统一」）

全部为系统自带素材，不是演示假数据：
  - 4 张 PPM(P6) 壁纸       : 真实像素，深浅风格统一，供图片查看器解码/缩放/设为壁纸
  - 24 帧 PPM(P6) 片源序列  : 真实逐帧像素，供视频播放器按单调时钟解码播放
  - 3 个 WAV(16bit PCM) 音频: 真实采样，供音乐播放器解析头信息并绘制波形

输出两处：
  1) /usr/share/aether/...                      本机 host 取证直接可读
  2) <stage>/usr/share/aether/...               随系统盘注入 ARM64 客机
路径与 aether-apps2.c 中的 WALL_DIR / CLIP_DIR / AUDIO_DIR 严格一致。
"""
import math
import os
import struct
import sys
import wave

HOST = "/usr/share/aether"
STAGE = "/root/aether-archive/aether-aarch64-build/stage/usr/share/aether"
# 版本文件由发布流程（build_apk.sh / make_system_v6.sh）维护，资产生成脚本
# 不得覆写，否则会把系统盘内版本倒退成历史值（此处曾误写 1.0.12-stage3）。


def write_ppm(path, w, h, pixel_fn):
    """写 P6 PPM：真实逐像素 RGB888。"""
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        row = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, b = pixel_fn(x, y, w, h)
                row += bytes((max(0, min(255, r)),
                              max(0, min(255, g)),
                              max(0, min(255, b))))
        f.write(row)


def clamp(v, lo=0, hi=255):
    return int(max(lo, min(hi, v)))


def wallpaper(style):
    """4 套风格统一、深浅分设的壁纸（渐变 + 柔和光晕 + 极细网格纹理）。"""
    def fn(x, y, w, h):
        u = x / w
        v = y / h
        # 对角渐变基色
        t = 0.55 * v + 0.45 * u
        if style == 0:      # 深蓝 · 极光
            r = 8 + 18 * t + 26 * math.exp(-((u - 0.72) ** 2 + (v - 0.24) ** 2) / 0.10)
            g = 20 + 52 * t + 40 * math.exp(-((u - 0.30) ** 2 + (v - 0.70) ** 2) / 0.14)
            b = 46 + 88 * t
        elif style == 1:    # 浅雾 · 晨光（浅色主题）
            # 基色整体压低一档，给同心波纹留足上下明暗余量，避免加亮侧被
            # clamp 到 255 削顶；仍是浅底，保证深色文字可读（总纲 5.5）。
            r = 188 + 34 * t
            g = 202 + 30 * t
            b = 218 + 22 * t
            glow = 12 * math.exp(-((u - 0.5) ** 2 + (v - 0.30) ** 2) / 0.16)
            r += glow; g += glow * 0.7
            # 柔和冷色纵深：与深色壁纸同一套「渐变 + 光晕 + 同心波纹」语言，
            # 只是换成浅色调，成套风格统一（总纲 6.6）。
            cool = 14 * math.exp(-((u - 0.28) ** 2 + (v - 0.72) ** 2) / 0.18)
            g += cool * 0.5; b += cool
        elif style == 2:    # 紫霞
            r = 40 + 66 * t
            g = 22 + 30 * t
            b = 74 + 78 * t
            glow = 30 * math.exp(-((u - 0.22) ** 2 + (v - 0.55) ** 2) / 0.12)
            r += glow; b += glow * 0.6
        else:               # 墨绿 · 极简
            r = 10 + 22 * t
            g = 34 + 60 * t
            b = 32 + 44 * t
        # 同心圆波纹：深色细密（振幅 5，沉稳），浅色加大振幅与周期对比
        # （13 + 6 双频），补偿上层磨砂玻璃遮罩的均化，让「底层清晰壁纸 /
        # 控件区磨砂」这一分层在浅色下同样可见（用户硬要求 2；总纲 6.1）。
        # 浅色基色已压低留余量，波纹不会削顶，也不会破坏可读性。
        d = math.hypot(u - 0.5, v - 0.42)
        if style == 1:
            ripple = 13 * math.sin(d * 30) + 6 * math.sin(d * 62)
        else:
            ripple = 5 * math.sin(d * 46)
        return (clamp(r + ripple), clamp(g + ripple), clamp(b + ripple))
    return fn


def clip_frame(idx, total):
    """开机动画语言：暗场中环形闭合 + 光点沿环游走 + 中心柔光（无旋转加载圈）。"""
    def fn(x, y, w, h):
        u, v = x / w, y / h
        cx, cy = 0.5, 0.46
        d = math.hypot((u - cx) * (w / h) * 1.0, v - cy)
        phase = idx / total
        # 背景：深蓝渐变
        r = 4 + 8 * v
        g = 8 + 18 * v
        b = 20 + 40 * v
        # 中心柔光随时间起伏
        glow = 26 * math.exp(-(d * d) / 0.02) * (0.5 + 0.5 * math.sin(phase * 6.283))
        r += glow * 0.4; g += glow * 0.7; b += glow
        # 环形（半径随帧轻微呼吸）
        R = 0.20 + 0.012 * math.sin(phase * 6.283)
        ring = math.exp(-((d - R) ** 2) / 0.00018)
        r += ring * 70; g += ring * 130; b += ring * 190
        # 光点沿环游走
        ang = math.atan2(v - cy, (u - cx) * (w / h) * 1.0)
        for k in range(4):
            a = (phase * 6.283 + k * 6.283 / 4) % 6.283
            da = abs(math.atan2(math.sin(ang - a), math.cos(ang - a)))
            spot = math.exp(-(da * da) / 0.006) * math.exp(-((d - R) ** 2) / 0.0012)
            r += spot * 150; g += spot * 190; b += spot * 230
        return (clamp(r), clamp(g), clamp(b))
    return fn


def write_wav(path, seconds, freq_fn, rate=8000):
    """写 16bit 单声道 PCM WAV：真实采样值（带包络，波形有动态）。"""
    n = int(seconds * rate)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        # 起落包络，避免恒定振幅
        env = min(1.0, t / 0.15, (seconds - t) / 0.25)
        env = max(0.0, env)
        s = freq_fn(t) * env * 0.62
        val = int(max(-1.0, min(1.0, s)) * 32767)
        frames += struct.pack("<h", val)
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(bytes(frames))


def main():
    roots = [HOST, STAGE]
    for root in roots:
        for sub in ("wallpapers", "clips/loop", "audio"):
            os.makedirs(os.path.join(root, sub), exist_ok=True)

    total_w = total_c = total_a = 0

    # 壁纸 360x640（contain 放大到 720x1280 画布，体积受控）
    W, H = 360, 640
    for i in range(4):
        name = "wall-%02d.ppm" % (i + 1)
        for root in roots:
            p = os.path.join(root, "wallpapers", name)
            write_ppm(p, W, H, wallpaper(i))
        total_w += os.path.getsize(os.path.join(HOST, "wallpapers", name))

    # 视频 24 帧 320x180
    CW, CH = 320, 180
    for i in range(24):
        name = "f%03d.ppm" % i
        for root in roots:
            p = os.path.join(root, "clips", "loop", name)
            write_ppm(p, CW, CH, clip_frame(i, 24))
        total_c += os.path.getsize(os.path.join(HOST, "clips", "loop", name))

    # 音频：三首真实合成的短曲
    notes = [
        (261.63, 329.63, 392.00),   # C 大三和弦
        (220.00, 277.18, 329.63),   # A 小三和弦
        (293.66, 369.99, 440.00),   # D 大三和弦
    ]
    songs = [
        ("overture.wav", 4.0, lambda t: sum(math.sin(2 * math.pi * f * t) for f in notes[int(t * 2) % 3]) / 3.0
         + 0.18 * math.sin(2 * math.pi * 523.25 * t) * (0.5 + 0.5 * math.sin(2 * math.pi * 2 * t))),
        ("datastream.wav", 3.0, lambda t: 0.6 * math.sin(2 * math.pi * (110 + 40 * math.sin(2 * math.pi * 0.7 * t)) * t)
         + 0.25 * math.sin(2 * math.pi * 880 * t) * (0.5 + 0.5 * math.sin(2 * math.pi * 3 * t))),
        ("chime.wav", 2.0, lambda t: 0.7 * math.sin(2 * math.pi * (659.25 if t < 0.6 else 880.0 if t < 1.2 else 1046.5) * t)),
    ]
    for name, secs, fn in songs:
        for root in roots:
            write_wav(os.path.join(root, "audio", name), secs, fn)
        total_a += os.path.getsize(os.path.join(HOST, "audio", name))

    print("壁纸 4 张 360x640 : %d 字节 (单份)" % total_w)
    print("片源 24 帧 320x180: %d 字节 (单份)" % total_c)
    print("音频 3 首 WAV     : %d 字节 (单份)" % total_a)
    print("单份合计          : %d 字节 (%.2f MB)" % (total_w + total_c + total_a,
                                                    (total_w + total_c + total_a) / 1048576.0))
    print("输出根目录        : %s , %s" % (HOST, STAGE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
