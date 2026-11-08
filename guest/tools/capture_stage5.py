#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_stage5.py - 阶段 5 取证：应用商店与安装闭环 / 小窗 / 多任务 / 息屏显示(AOD)

驱动 x86 host 合成器（逻辑画布 720x1280），经 IPC 现场跑通《总纲》第九章验收 5
要求的：应用商店首页/详情/安装闭环（装一个应用→桌面出现→打开）、小窗浮窗
（拖动/缩放/侧边吸附/第二请求反馈）、多任务卡片、息屏显示 AOD。所有画面来自
合成器真实渲染，无演示数据。深浅两套各一组。
"""
import os
import socket
import subprocess
import time

from PIL import Image

COMP = "/root/aether-archive/aether-pc-build/root/sbin/aether-compositor"
SOCK = "/run/aetheros-comp.sock"
OUT  = "/workspace/aether-pc/ui-shots/stage5"
os.makedirs(OUT, exist_ok=True)

ENV = dict(os.environ)
ENV["AETHER_RES"] = "720x1280"
ENV["AETHER_PHONE"] = "1"
ENV["AETHER_HEADLESS"] = "1"

THEMES = [(0, "dark"), (1, "light")]

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

def state():
    return ipc("state")

def run():
    print("启动合成器:", COMP)
    proc = subprocess.Popen([COMP], env=ENV)
    # 等待 socket
    for _ in range(50):
        try:
            ipc("ping")
            break
        except Exception:
            time.sleep(0.1)
    print("IPC ping:", ipc("ping"))

    seq = 0
    def step(name, theme_tag, acts, settle=True):
        nonlocal seq
        seq += 1
        for a in acts:
            ipc(a)
        if settle:
            ipc("settle")
        shot("%02d-%s-%s" % (seq, theme_tag, name))

    n = 0
    for theme, tag in THEMES:
        n += 1
        ipc("skipboot")                 # 跳过开机进锁屏
        ipc("unlock")                   # 解锁进桌面
        ipc("theme %d" % theme)
        ipc("settle")
        # 桌面（商店已在 Dock）
        step("home", tag, [])
        # 打开应用商店（APP_STORE=14）
        ipc("open 14"); ipc("settle")
        step("store-home", tag, [])
        # 商店详情（笔记索引 0，可安装）
        ipc("appcmd store detail 0"); ipc("settle")
        step("store-detail-notes", tag, [])
        # 安装笔记（fast：确定性完成）
        ipc("appcmd store installfast 0"); ipc("settle")
        step("store-installed", tag, [])
        # 首开权限授予 -> 打开（先截图笔记已打开，再返回，避免 back 在截图前执行）
        ipc("appcmd store permallow"); ipc("settle")
        step("notes-opened", tag, [])
        ipc("back")                      # 回到商店/桌面
        # 回桌面，验证笔记已出现在桌面
        ipc("home"); ipc("page 3"); ipc("settle")
        step("home-notes-installed", tag, [])
        # 抽屉可见笔记（先截图抽屉，再关闭，避免 drawer 0 在截图前执行）
        ipc("drawer 1"); ipc("settle")
        step("drawer-with-notes", tag, [])
        ipc("drawer 0")
        # 多任务卡片（先开几个应用）
        ipc("open 4"); ipc("open 8"); ipc("open 11"); ipc("settle")  # 终端/浏览器/音乐
        ipc("recents"); ipc("settle")
        step("recents", tag, [])   # 先截图多任务卡片，再返回
        ipc("home")
        # 小窗：打开浏览器小窗
        ipc("float 8"); ipc("settle")
        step("float-browser", tag, [])
        # 第二请求反馈（小窗已存在）
        r = ipc("float 11")
        print("  第二小窗请求:", r)
        ipc("settle")
        step("float-second-request", tag, [])
        # 关闭小窗
        ipc("closefloat"); ipc("settle")
        step("float-closed", tag, [])
        # 息屏显示 AOD（默认开启 -> 极低亮度暗显）
        ipc("sleep"); ipc("settle")
        step("aod", tag, [])
        # 关闭 AOD（开关测试）：先 wake 回桌面，关 SYS_AOD=20，再 sleep 进入息屏
        ipc("wake"); ipc("settle")
        ipc("sys 20 0")   # SYS_AOD=20 -> 0 关闭
        ipc("sleep"); ipc("settle")
        # 截图前不再执行还原动作（step 的 acts 在截图前运行），此时处于
        # PH_AOD 且 SYS_AOD=0 -> draw_aod 直接返回全黑。
        step("aod-off-black", tag, [])
        # 还原：唤醒 + 重新开启 AOD，准备后续步骤
        ipc("wake"); ipc("settle")
        ipc("sys 20 1")
        ipc("settle")
        # 卸载笔记（回到桌面后从商店卸载）
        ipc("open 14"); ipc("appcmd store uninstall 0"); ipc("settle")
        ipc("home"); ipc("page 3"); ipc("settle")
        step("home-after-uninstall", tag, [])
        ipc("home")  # 复位

    # 汇总关键状态
    ipc("unlock")
    ipc("open 14")
    print("\n最终状态:", state())
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    print("\n取证帧数:", seq * 2, "输出目录:", OUT)

if __name__ == "__main__":
    run()
