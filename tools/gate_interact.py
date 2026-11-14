#!/usr/bin/env python3
# gate_interact.py - A 轨交互/活体/可读性门禁（round-012）
#
# 用真实输入路径（合成器 debug TCP 通道 :9230，与物理触摸同一 UI handler）驱动设备，
# 并复用 QMP screendump 抓真实帧，对复检报告 04:11 提出的 R1/R2/R3/R4/R5 做机器可检断言：
#   R1 锁屏文字可读（首帧文字带 Laplacian 方差高于阈值）
#   R2 界面在动（无输入隔 2.5s 两帧，状态栏区域出现像素差异 -> 时钟秒位走动）
#   R3 时区由配置决定（aether.tz=UTC 与默认 CST-8 两次启动，clock 小时差 = 8 mod 24）
#   R4 电量绑定真实数据源（默认无电源节点 -> 未就绪；battery 注入 -> N%，非写死 100%）
#   R5 接受真实触摸（TCP click 走与触摸同一 handler，点击前后两帧出现显著差异）
#
# 取证产物（PNG/JSON）写入 AETHER_EVIDENCE（默认 evidence/round-012）。
import os, sys, json, socket, subprocess, time, signal
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, "build/aarch64")
IMAGE = os.path.join(BUILD, "Image")
ROOTFS = os.path.join(BUILD, "rootfs.ext4")
QEMU = "qemu-system-aarch64"
TCP_PORT = 9230

# 阈值
R1_SHARPNESS_MIN = 60.0      # 文字带 Laplacian 方差下限（1:1 原分辨率应远高于此）
R2_MIN_PIXELS = 200          # 状态栏区域差异像素下限（秒位走动）
R5_MIN_PIXELS = 5000         # 点击前后全帧差异像素下限（锁屏->桌面应很大）

def log(*a):
    print("[gate-interact]", *a, flush=True)


def launch_qemu(evid, tz=None, timeout=100):
    os.makedirs(evid, exist_ok=True)
    logpath = os.path.join(evid, "boot-serial.log")
    qmp = os.path.join(evid, "aether-qmp.sock")
    for p in (logpath, qmp):
        try: os.remove(p)
        except FileNotFoundError: pass
    append = "root=/dev/vda rw console=ttyAMA0 init=/init panic=5 loglevel=7 video=720x1280 aether.input_tcp=1"
    if tz:
        append += " aether.tz=%s" % tz
    dev_extra = ["-device", "virtio-net-pci,netdev=n0",
                 "-netdev", "user,id=n0,hostfwd=tcp::%d-:9230" % TCP_PORT]
    with open(logpath, "w") as lf:
        proc = subprocess.Popen(
            [QEMU, "-M", "virt", "-cpu", "cortex-a57", "-m", "512M", "-smp", "2",
             "-kernel", IMAGE, "-drive", "file=%s,format=raw,if=virtio" % ROOTFS,
             "-append", append, "-device", "virtio-gpu-pci"] + dev_extra + [
             "-display", "none", "-serial", "file:%s" % logpath, "-monitor", "none",
             "-no-reboot", "-qmp", "unix:%s,server=on,wait=off" % qmp],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 等待开机链路完成
    ok = False
    for _ in range(timeout):
        try:
            with open(logpath) as f:
                if "AETHEROS_BOOT_DIAG_DONE=1" in f.read():
                    ok = True; break
        except FileNotFoundError:
            pass
        if proc.poll() is not None:
            break
        time.sleep(1)
    if not ok:
        log("[ERROR] 开机诊断未完成"); proc.kill(); raise SystemExit(1)
    time.sleep(3)  # 给合成器首帧渲染时间
    return proc, qmp, logpath


def qmp_connect(qmp):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(qmp); s.settimeout(10)
    f = s.makefile("rwb")
    f.readline()
    f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode()); f.flush()
    while True:
        o = json.loads(f.readline().decode())
        if "return" in o: break
    return s, f

def qmp_exec(f, cmd, args=None):
    m = {"execute": cmd}
    if args: m["arguments"] = args
    f.write((json.dumps(m) + "\n").encode()); f.flush()
    while True:
        o = json.loads(f.readline().decode())
        if "return" in o or "error" in o: return o

def screendump(f, qmp_sock_path, out_ppm):
    qmp_exec(f, "screendump", {"filename": out_ppm})


def ppm_to_png(ppm, png):
    Image.open(ppm).convert("RGB").save(png, "PNG")


def frame_diff(a, b, thr=40):
    ia = Image.open(a).convert("RGB"); ib = Image.open(b).convert("RGB")
    pa = ia.tobytes(); pb = ib.tobytes(); n = len(pa) // 3; ch = 0
    # 采样步长加速（每 4 像素）
    for i in range(0, len(pa), 12):
        if abs(pa[i]-pb[i]) + abs(pa[i+1]-pb[i+1]) + abs(pa[i+2]-pb[i+2]) > thr:
            ch += 1
    return ch, n // 4


def band_sharpness(ppm, y0r=0.18, y1r=0.55):
    im = Image.open(ppm).convert("L")
    W, H = im.size
    arr = im.load()
    band = []
    y0, y1 = int(H*y0r), int(H*y1r)
    for y in range(y0, y1):
        for x in range(0, W, 2):
            band.append(arr[x, y])
    # Laplacian 方差
    import statistics
    lap = []
    for i in range(1, len(band)-1):
        lap.append(abs(band[i-1] - 2*band[i] + band[i+1]))
    if not lap: return 0.0
    m = sum(lap)/len(lap)
    var = sum((v-m)**2 for v in lap)/len(lap)
    return var


def band_diff(a, b, y0r, y1r, thr=30):
    """计算指定纵向区域（状态栏）的像素差异，用于证明锁定后时钟秒位仍在刷新。"""
    ia = Image.open(a).convert("RGB"); ib = Image.open(b).convert("RGB")
    W, H = ia.size
    pa = ia.tobytes(); pb = ib.tobytes()
    y0, y1 = int(H*y0r), int(H*y1r)
    stride = W * 3
    ch = 0
    for y in range(y0, y1):
        base = y * stride
        for x in range(0, W, 2):
            i = base + x * 3
            if abs(pa[i]-pb[i]) + abs(pa[i+1]-pb[i+1]) + abs(pa[i+2]-pb[i+2]) > thr:
                ch += 1
    return ch


def tcp_send(line):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", TCP_PORT))
    s.sendall((line + "\n").encode())
    time.sleep(0.2)
    data = b""
    try:
        while True:
            c = s.recv(4096)
            if not c: break
            data += c
            if data.endswith(b"\n"): break
    except socket.timeout:
        pass
    s.close()
    return data.decode(errors="replace").strip()


def main():
    evid = os.environ.get("AETHER_EVIDENCE", os.path.join(REPO, "evidence/round-012"))
    evid = os.path.abspath(evid)
    os.makedirs(evid, exist_ok=True)
    results = []

    # ---------- 第一次启动：默认时区（CST-8），收集 R1/R2/R4/R5 ----------
    log("启动 QEMU（默认时区，gate 模式）...")
    proc, qmp, logpath = launch_qemu(evid)
    try:
        s, f = qmp_connect(qmp)
        def cap(name):
            ppm = os.path.join(evid, name + ".ppm")
            png = os.path.join(evid, name + ".png")
            screendump(f, qmp, ppm); time.sleep(0.25)
            ppm_to_png(ppm, png)
            return png
        # 等待合成器 serving，再多等开机动画结束、锁屏时钟开始走动
        for _ in range(10):
            with open(logpath) as lf:
                if "serving on" in lf.read(): break
            time.sleep(1)
        time.sleep(4)

        # R1：首帧文字带锐度
        A = cap("r1_lock")
        r1 = band_sharpness(os.path.join(evid, "r1_lock.ppm"))
        results.append(("R1 锁屏文字可读", r1 >= R1_SHARPNESS_MIN,
                        "文字带 Laplacian 方差=%.1f (阈值>=%.0f)" % (r1, R1_SHARPNESS_MIN)))

        # R2：界面在动（时钟在走）。静态锁屏大时钟只显示 HH:MM（分钟级才变），
        # 因此用状态栏秒位 + clock IPC 双证：①同启动内两次 clock 字符串不同；
        # ②顶部状态栏区域（含秒位）隔 3s 抓帧出现像素差异。
        clk_a = tcp_send("clock")
        a2 = cap("r2a"); time.sleep(3.0); b2 = cap("r2b")
        clk_b = tcp_send("clock")
        ca = cb = None
        try: ca = json.loads(clk_a)["clock"]
        except Exception: pass
        try: cb = json.loads(clk_b)["clock"]
        except Exception: pass
        d2 = band_diff(os.path.join(evid, "r2a.ppm"), os.path.join(evid, "r2b.ppm"), 0.02, 0.08)
        r2 = (ca != cb) and (d2 >= 3)
        results.append(("R2 界面在动(时钟秒位刷新)", r2,
                        "clock %s->%s(不同) 状态栏帧差=%d(阈值>=3)" % (ca, cb, d2)))

        # R4：电量默认（无电源节点 -> 未就绪）
        bi0 = tcp_send("batinfo")
        default_text = ""
        try: default_text = json.loads(bi0).get("battery_text", "")
        except Exception: pass
        r4a = (default_text == "未就绪")
        # 注入 42% 后应为 42%
        tcp_send("battery 42")
        time.sleep(0.5)
        bi1 = tcp_send("batinfo")
        inj_text = ""
        try: inj_text = json.loads(bi1).get("battery_text", "")
        except Exception: pass
        r4b = (inj_text == "42%")
        results.append(("R4 电量绑定真实数据源(非写死100%)", r4a and r4b,
                        "默认=%r(无源->未就绪) 注入42后=%r" % (default_text, inj_text)))

        # R5：真实输入（TCP click，与触摸同一 handler）-> 锁屏->桌面，帧显著变化
        cap("r5_before")
        tcp_send("click 360 640")
        time.sleep(1.5)
        cap("r5_after")
        d5, n5 = frame_diff(os.path.join(evid, "r5_before.ppm"), os.path.join(evid, "r5_after.ppm"))
        results.append(("R5 接受真实触摸(点击解锁)", d5 >= R5_MIN_PIXELS,
                        "点击前后全帧差异像素=%d (阈值>=%d)" % (d5, R5_MIN_PIXELS)))

        # R3 默认时钟（CST-8）
        clk0 = tcp_send("clock")
        h0 = None
        try: h0 = int(json.loads(clk0)["clock"].split(":")[0])
        except Exception: pass

        qmp_exec(f, "quit")
        s.close()
    finally:
        proc.kill(); proc.wait()

    # ---------- 第二次启动：aether.tz=UTC，收集 R3 ----------
    log("启动 QEMU（aether.tz=UTC）...")
    evid2 = evid + "_tzutc"
    proc2, qmp2, logpath2 = launch_qemu(evid2, tz="UTC")
    try:
        s2, f2 = qmp_connect(qmp2)
        for _ in range(10):
            with open(logpath2) as lf:
                if "serving on" in lf.read(): break
            time.sleep(1)
        clk1 = tcp_send("clock")
        h1 = None
        try: h1 = int(json.loads(clk1)["clock"].split(":")[0])
        except Exception: pass
        r3 = (h0 is not None and h1 is not None and ((h0 - h1) % 24 == 8))
        results.append(("R3 时区由配置决定(CST-8 vs UTC 差8h)", r3,
                        "默认=%s(小时=%s) UTC=%s(小时=%s) 差=%s" % (clk0, h0, clk1, h1, ((h0-h1)%24) if (h0 is not None and h1 is not None) else "NA")))
        qmp_exec(f2, "quit"); s2.close()
    finally:
        proc2.kill(); proc2.wait()

    # ---------- 汇总 ----------
    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print("\n=== 交互/活体/可读性门禁 合计：%d/%d ===" % (passed, total))
    for name, ok, detail in results:
        print("| %s | %s | %s |" % (name, "PASS" if ok else "FAIL", detail))
    out = {"passed": passed, "total": total,
           "results": [{"name": n, "passed": ok, "detail": d} for n, ok, d in results]}
    with open(os.path.join(evid, "gate_interact.json"), "w") as fo:
        json.dump(out, fo, ensure_ascii=False, indent=2)
    print("JSON ->", os.path.join(evid, "gate_interact.json"))
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
