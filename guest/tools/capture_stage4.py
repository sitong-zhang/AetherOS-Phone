#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_stage4.py - 阶段 4 host 界面取证

直接启动 x86 headless 合成器（逻辑画布 720x1280），经 Unix 域套接字发送
与真机点击共用同一套 handler 的 IPC 命令，现场跑通《总纲》第九章验收 4
要求的灵动岛五个场景（音乐 / 计时器 / 录音 / 充电 / 导航）、三种形态、
主副活动并存与互换、隐私指示点与使用面板、全局搜索分组与空状态、
负一屏，并给出深浅两套截图。所有画面来自合成器真实渲染，无演示数据。
"""
import os
import socket
import subprocess
import time

from PIL import Image

COMP = "/root/aether-archive/aether-pc-build/root/sbin/aether-compositor"
SOCK = "/run/aetheros-comp.sock"
OUT = "/workspace/aether-pc/ui-shots/stage4"

ENV = dict(os.environ)
ENV["AETHER_RES"] = "720x1280"
ENV["AETHER_PHONE"] = "1"


def ipc(line, timeout=8.0):
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
    print("  %-34s %s 非黑 %.1f%%" % (name, im.size, nb / len(px) * 100))
    return True


def cmd(*parts):
    return ipc(" ".join(str(p) for p in parts))


def island(scene):
    cmd("islandclear")
    return cmd("island", scene)


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
                            stderr=open("/tmp/aether-comp-stage4.log", "wb"))
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

    frames = 0
    try:
        cmd("unlock")
        cmd("home")
        shot("01-home-dark")
        frames += 1

        # ---- 灵动岛：三种形态（音乐） ----
        island("music")
        cmd("islandform", 0)
        shot("02-island-music-collapsed")
        frames += 1
        cmd("islandform", 1)
        shot("03-island-music-expanded")
        frames += 1
        cmd("islandform", 2)
        shot("04-island-music-card")
        frames += 1

        # ---- 验收 4：五个场景现场跑通 ----
        for scene, label in [("music", "05-island-music"),
                             ("timer", "06-island-timer"),
                             ("record", "07-island-record"),
                             ("charge", "08-island-charge"),
                             ("nav", "09-island-nav")]:
            island(scene)
            cmd("islandform", 2)
            # 充电是短暂事件，截图前再触发一次确保仍在显示
            if scene == "charge":
                cmd("island", "charge")
            shot(label)
            frames += 1

        # ---- 主活动 + 副活动并存，点击互换 ----
        cmd("islandclear")
        cmd("island", "call")
        cmd("island", "music")
        cmd("islandform", 1)
        shot("10-island-primary-sub")
        frames += 1
        cmd("islandswap")
        shot("11-island-swapped")
        frames += 1
        print("  主副状态:", cmd("state")[:220])

        # ---- 隐私指示：绿点（相机/麦克风）+ 橙点（定位）----
        cmd("islandclear")
        cmd("privacy", 1, "相机")
        cmd("privacy", 3, "地图")
        shot("12-privacy-dots")
        frames += 1
        # 真机路径：点击状态栏右侧指示点弹出面板
        cmd("click", 690, 20)
        shot("13-privacy-panel")
        frames += 1
        print("  隐私面板状态:", cmd("state")[:220])
        # 面板内「关闭」按钮直接停止权限使用
        cmd("click", 630, 112)
        shot("14-privacy-closed")
        frames += 1
        cmd("privacy", 0)

        # ---- 全局搜索：输入即出结果、分组、无结果空状态 ----
        cmd("search", "open")
        shot("15-search-suggest")
        frames += 1
        cmd("search", "文件")
        shot("16-search-file")
        frames += 1
        cmd("search", "显示")
        shot("17-search-setting")
        frames += 1
        cmd("search", "不存在的关键词zzz")
        shot("18-search-empty")
        frames += 1
        print("  搜索状态:", cmd("state")[:260])
        cmd("search", "close")

        # ---- 负一屏：卡片纵向排列、真实数据源 ----
        cmd("neg1", 1)
        shot("19-neg1-dark")
        frames += 1
        cmd("neg1", 0)

        # ---- 浅色主题一套 ----
        cmd("theme", 1)
        island("music")
        cmd("islandform", 1)
        shot("20-island-light")
        frames += 1
        cmd("islandclear")
        cmd("search", "open")
        cmd("search", "日历")
        shot("21-search-light")
        frames += 1
        cmd("search", "close")
        cmd("neg1", 1)
        shot("22-neg1-light")
        frames += 1
        cmd("neg1", 0)
        shot("23-home-light")
        frames += 1
        cmd("theme", 0)

        # ---- 既有能力回归 ----
        cmd("panel", 2)
        shot("24-control-center")
        frames += 1
        cmd("panel", 0)
        cmd("drawer", 1)
        shot("25-drawer-14apps")
        frames += 1
        cmd("drawer", 0)
        cmd("home")

        print("最终状态:", cmd("state")[:300])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("取证帧数:", frames)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
