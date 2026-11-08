#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_stage3.py - 阶段 3 host 界面取证

直接启动 x86 headless 合成器（逻辑画布 720x1280），经 Unix 域套接字发送
与真机点击共用同一套 handler 的 IPC 命令，逐个打开 14 个内置应用、
设置二级页、控制中心、深浅主题与真实壁纸，导出 PPM 并转 PNG。
不使用演示数据：所有画面来自合成器真实渲染。
"""
import os
import socket
import subprocess
import sys
import time

from PIL import Image

COMP = "/root/aether-archive/aether-pc-build/root/sbin/aether-compositor"
SOCK = "/run/aetheros-comp.sock"
OUT = "/workspace/aether-pc/ui-shots/stage3"

ENV = dict(os.environ)
ENV["AETHER_RES"] = "720x1280"
ENV["AETHER_PHONE"] = "1"

APP = {
    "files": 0, "terminal": 1, "editor": 2, "calc": 3,
    "monitor": 4, "settings": 5, "about": 6, "modules": 7,
    "browser": 8, "gallery": 9, "video": 10, "music": 11,
    "clock": 12, "calendar": 13,
}


def ipc(line, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall((line + "\n").encode("utf-8"))
    buf = b""
    while True:
        try:
            chunk = s.recv(8192)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            break
    s.close()
    return buf.decode("utf-8", "replace").strip()


def shot(name):
    path = os.path.join(OUT, name + ".ppm")
    r = ipc("screenshot " + path)
    if '"ok":true' not in r:
        print("  !! screenshot failed:", name, r)
        return False
    im = Image.open(path).convert("RGB")
    im.save(os.path.join(OUT, name + ".png"))
    px = list(im.getdata())
    nb = sum(1 for c in px if c[0] + c[1] + c[2] > 40)
    print("  %-30s %s 非黑 %.1f%%" % (name, im.size, nb / len(px) * 100))
    return True


def cmd(*parts):
    return ipc(" ".join(str(p) for p in parts))


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".ppm") or f.endswith(".png"):
            os.unlink(os.path.join(OUT, f))

    if os.path.exists(SOCK):
        os.unlink(SOCK)
    print("启动合成器:", COMP)
    proc = subprocess.Popen([COMP], env=ENV,
                            stdout=subprocess.DEVNULL,
                            stderr=open("/tmp/aether-comp-stage3.log", "wb"))
    for _ in range(60):
        if os.path.exists(SOCK):
            break
        time.sleep(0.1)
    if not os.path.exists(SOCK):
        print("!! 合成器套接字未就绪")
        proc.terminate()
        return 1
    time.sleep(0.4)
    print("IPC ping:", ipc("ping"))

    try:
        # 开机 -> 解锁 -> 桌面
        cmd("unlock")
        cmd("home")
        shot("01-home-dark")

        # 14 个内置应用逐个打开取证
        for name, k in APP.items():
            cmd("open", k)
            shot("app-%02d-%s" % (k, name))
            cmd("back")

        # 设置：一级列表 + 关键二级页
        cmd("open", APP["settings"])
        shot("20-settings-top")
        for p, label in [(3, "display"), (4, "sound"), (5, "battery"),
                         (6, "storage"), (7, "apps"), (9, "privacy"),
                         (13, "accessibility"), (16, "about")]:
            cmd("appcmd page", p)
            shot("21-set-%02d-%s" % (p, label))
        cmd("appcmd back")
        cmd("back")

        # 浏览器：系统信息页
        cmd("open", APP["browser"])
        cmd("appcmd", "about:system")
        shot("30-browser-system")
        cmd("back")

        # 图片查看器：设壁纸 -> 回桌面生效
        cmd("open", APP["gallery"])
        shot("31-gallery")
        cmd("click", 660, 78)     # 顶栏右侧「设为壁纸」
        cmd("home")
        shot("32-home-wallpaper")

        # 视频 / 音乐 / 时钟三态 / 日历
        cmd("open", APP["video"])
        cmd("appcmd frame", 8)
        shot("33-video")
        cmd("back")
        cmd("open", APP["music"])
        shot("34-music")
        cmd("back")
        cmd("open", APP["clock"])
        shot("35-clock-alarm")
        cmd("appcmd timer", 95)
        shot("36-clock-timer")
        cmd("appcmd tab", 2)
        cmd("appcmd sw")
        shot("37-clock-stopwatch")
        cmd("back")
        cmd("open", APP["calendar"])
        shot("38-calendar")
        cmd("back")

        # 控制中心 / 通知中心 / 抽屉 / 浅色主题
        cmd("panel", 2)
        shot("40-control-center")
        cmd("panel", 1)
        shot("41-notif-center")
        cmd("panel", 0)
        cmd("drawer", 1)
        shot("42-drawer-14apps")
        cmd("drawer", 0)
        cmd("theme", 1)
        shot("43-home-light")
        cmd("open", APP["settings"])
        cmd("appcmd page", 3)
        shot("44-settings-light")
        cmd("back")
        cmd("theme", 0)
        cmd("home")

        print("最终状态:", ipc("state"))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        if os.path.exists(SOCK):
            os.unlink(SOCK)

    pngs = sorted(f for f in os.listdir(OUT) if f.endswith(".png"))
    print("取证帧数:", len(pngs))
    return 0 if pngs else 1


if __name__ == "__main__":
    sys.exit(main())
