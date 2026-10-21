#!/usr/bin/env python3
"""AetherOS 客机内「系统更新」应用端到端冒烟（#294 / P5）。

链路（全部真实组件，无 mock 客机侧代码）：
  QMP abs 事件 -> 客机 evdev(virtio-tablet) -> compositor tap_route
  -> create sock= 注册的 UDS 转发 -> aether-updater 命中测试
  -> HTTP 10.0.2.2:<aether_port> -> 宿主 mock 更新服务

断言链：
  U1 boot diag 完成 + 服务拓扑 0 失败
  U2 pkginstall 真实安装 updater.aepkg（串口日志）
  U3 aether-pkg list 含 aetheros.updater（应用网格索引）
  U4 sysuictl launch aetheros.updater -> 窗口注册，list 显示 sock=
  U5 tap「检查更新」-> 宿主收到 GET /api/update-check
  U6 tap「立即更新」-> 宿主收到 POST /api/update
  U7 客机串口出现 updater 的窗口/tap-sock 自报行

宿主 mock：127.0.0.1:18080 的 HTTP 服务（QEMU user 网络即 10.0.2.2:18080），
   GET  /api/update-check -> {"current":"phone-v8","latest":"phone-v9","has_update":true}
   POST /api/update       -> {"started":true}
"""
import argparse
import glob
import http.server
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------- 报告

PASSED = []
FAILED = []
SKIPPED = []


def record(name, ok, detail=""):
    (PASSED if ok else FAILED).append((name, detail))
    print(("[PASS] " if ok else "[FAIL] ") + name + (f" —— {detail}" if detail else ""), flush=True)


def skip(name, detail=""):
    SKIPPED.append((name, detail))
    print("[SKIP] " + name + (f" —— {detail}" if detail else ""), flush=True)


def tail(text, n=600):
    return text[-n:].replace("\n", " | ")


# ---------------------------------------------------------------- 宿主 mock 更新服务

MOCK_PORT = 18080
MOCK_HITS = []


class MockUpdateHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # 静音默认访问日志
        pass

    def _json(self, obj):
        # separators=(",", ":")：紧凑无空格——与宿主 AetherCore.checkUpdate()
        # 手写拼接的 JSON 逐字一致（客机解析器按此格式匹配字段）
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        MOCK_HITS.append(("GET", self.path))
        if self.path.startswith("/api/update-check"):
            self._json({"current": "phone-v8", "latest": "phone-v9",
                        "has_update": True})
        else:
            self._json({"ok": False})

    def do_POST(self):
        MOCK_HITS.append(("POST", self.path))
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length:
            self.rfile.read(length)
        if self.path.startswith("/api/update"):
            self._json({"started": True})
        else:
            self._json({"ok": False})


def start_mock_server():
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockUpdateHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


# ---------------------------------------------------------------- 客机控制台（同 boot-smoke 约定）

class GuestConsole:
    def __init__(self, log_path):
        self.log_path = log_path
        self.fifo_path = log_path + ".console.in"
        for p in (self.fifo_path,):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        os.mkfifo(self.fifo_path)
        self.fin = os.open(self.fifo_path, os.O_RDWR)
        self._logf = open(log_path, "wb")

    def read_log(self):
        try:
            with open(self.log_path, "rb") as f:
                return f.read().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def send(self, line):
        os.write(self.fin, (line + "\n").encode())

    def wait(self, marker, timeout, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = self.read_log()
            if marker in data[since:]:
                return True, data
            time.sleep(1.0)
        return False, self.read_log()

    def wait_line(self, marker, timeout, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = self.read_log()
            if any(l.strip() == marker for l in data[since:].splitlines()):
                time.sleep(0.15)
                data = self.read_log()
                return True, data
            time.sleep(0.5)
        return False, self.read_log()

    def mark(self):
        return len(self.read_log())


# ---------------------------------------------------------------- QMP

class QMP:
    def __init__(self, sock_path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.s.connect(sock_path)
                break
            except OSError:
                time.sleep(0.2)
        self.s.settimeout(30)
        self.f = self.s.makefile("rwb")

    def _read_msg(self):
        try:
            line = self.f.readline()
        except Exception:
            return None
        if not line:
            return None
        return json.loads(line.decode())

    def handshake(self):
        m = self._read_msg()
        if m is None or "QMP" not in m:
            return False
        self.f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
        self.f.flush()
        while True:
            m = self._read_msg()
            if m is None:
                return False
            if "return" in m:
                return True
            if "error" in m:
                return False

    def send_pointer(self, x, y, button="left"):
        """绝对指针点击（逻辑坐标 [0,0x7FFF]，与 virtio-tablet absmax 一致）。"""
        self.f.write((json.dumps({
            "execute": "input-send-event",
            "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": x}},
                {"type": "abs", "data": {"axis": "y", "value": y}},
                {"type": "btn", "data": {"down": True, "button": button}},
                {"type": "btn", "data": {"down": False, "button": button}},
            ]},
        }) + "\n").encode())
        self.f.flush()
        while True:
            m = self._read_msg()
            if m is None:
                return False
            if "return" in m:
                return True
            if "error" in m:
                sys.stderr.write("[QMP input-send-event error] %s\n" % json.dumps(m))
                return False


# ---------------------------------------------------------------- 几何换算

# 冒烟默认 1280x800。updater 是普通窗口（非 fs）：SystemUI(fs sys) 之后创建，
# win_count=2 -> x=40+(1%4)*40=80, y=40+(1%4)*30=70；内容区偏移 (+2,+24)。
WIN_X, WIN_Y = 80, 70
CONTENT_DX, CONTENT_DY = WIN_X + 2, WIN_Y + 24
# 按钮（窗口局部，见 aether-updater.c）：检查更新 (20,210)-(220,250)；
# 立即更新 (260,210)-(460,250)。
BTN_CHECK = (120, 230)
BTN_UPDATE = (360, 230)


def screen_to_tablet(sx, sy, xres, yres):
    return (int(sx / xres * 0x7FFF), int(sy / yres * 0x7FFF))


# ---------------------------------------------------------------- 主流程

def main():
    global con
    ap = argparse.ArgumentParser(description="AetherOS 客机内系统更新应用冒烟")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--rootfs", required=True)
    ap.add_argument("--log", default="/tmp/aether-updater-smoke.log")
    ap.add_argument("--summary", default="/tmp/aether-updater-smoke.summary.txt")
    ap.add_argument("--qemu", default="qemu-system-aarch64")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--cmd-timeout", type=int, default=60)
    ap.add_argument("--xres", type=int, default=1280)
    ap.add_argument("--yres", type=int, default=800)
    args = ap.parse_args()

    mock = start_mock_server()
    print(f"[mock] 宿主更新服务于 127.0.0.1:{MOCK_PORT}（客机视角 10.0.2.2:{MOCK_PORT}）", flush=True)

    con = GuestConsole(args.log)
    cmd = [
        args.qemu, "-M", "virt", "-cpu", "cortex-a57", "-smp", "2", "-m", "512M",
        "-kernel", args.kernel,
        "-drive", f"if=virtio,format=raw,file={args.rootfs}",
        # aether_port：客机 updater 的回连通道（宿主 launchQemu 注入同款参数）
        "-append", f"console=ttyAMA0 root=/dev/vda rw ip=dhcp aether_port={MOCK_PORT}",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0",
        "-device", f"virtio-gpu-pci,xres={args.xres},yres={args.yres}",
        "-device", "virtio-keyboard-pci",
        "-device", "virtio-tablet-pci",
        "-chardev", f"socket,path={args.log}.qmp,server=on,wait=off,id=qmp",
        "-mon", "chardev=qmp,mode=control",
        "-snapshot", "-display", "none", "-serial", "stdio", "-monitor", "none",
    ]
    print("[qemu] " + " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdin=con.fin, stdout=con._logf, stderr=con._logf)
    try:
        run_checks(args, proc)
    except Exception as e:
        record("X0 冒烟驱动未预期异常", False, f"{type(e).__name__}: {e}")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        con._logf.close()
        os.close(con.fin)
        try:
            os.unlink(con.fifo_path)
        except FileNotFoundError:
            pass
        mock.shutdown()
        with open(args.summary, "w") as f:
            f.write(summary_text())
        print(f"[smoke] summary -> {args.summary}")
    return 0 if not FAILED else 1


def summary_text():
    lines = ["AetherOS 客机内「系统更新」冒烟", "=" * 40]
    for n, d in PASSED:
        lines.append(f"[PASS] {n}" + (f" —— {d}" if d else ""))
    for n, d in FAILED:
        lines.append(f"[FAIL] {n}" + (f" —— {d}" if d else ""))
    for n, d in SKIPPED:
        lines.append(f"[SKIP] {n}" + (f" —— {d}" if d else ""))
    lines.append(f"result={len(PASSED)} passed / {len(FAILED)} failed / {len(SKIPPED)} skipped")
    return "\n".join(lines) + "\n"


def run_checks(args, proc):
    # ---- U1 启动取证 ----
    found, data = con.wait("AETHEROS_BOOT_DIAG_DONE", args.boot_timeout)
    record("U1 boot diag 完成", found, "" if found else tail(data))
    if not found:
        return
    m = re.search(r"service startup complete \((\d+) services, (\d+) failed\)", data)
    record("U1b 服务拓扑 0 失败", bool(m) and m.group(2) == "0",
           m.group(0) if m else "未找到 startup complete 行")

    # ---- U2 pkginstall 真实安装 updater.aepkg ----
    record("U2 pkginstall 安装 updater.aepkg",
           "updater.aepkg" in data and "pkginstall: done" in data,
           "串口未见 updater.aepkg 安装记录" if "updater.aepkg" not in data else "")

    # ---- B 登录 ----
    found, data = con.wait("login:", args.cmd_timeout)
    record("U3 getty 登录提示", found, "" if found else tail(data))
    if not found:
        return
    con.send("root")
    time.sleep(1.0)
    con.send("")
    time.sleep(1.0)
    mark = con.mark()
    con.send("echo LOGIN_MARKER_OK")
    found, data = con.wait_line("LOGIN_MARKER_OK", args.cmd_timeout, since=mark)
    record("U3b shell 回显", found, "" if found else tail(data, mark))
    if not found:
        return

    # ---- U4 aether-pkg list 含 aetheros.updater ----
    mark = con.mark()
    con.send("aether-pkg list; echo PKGLIST_DONE")
    found, data = con.wait_line("PKGLIST_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    record("U4 aether-pkg list 含 aetheros.updater",
           found and "aetheros.updater" in seg,
           "" if (found and "aetheros.updater" in seg) else tail(seg, mark if False else 0))

    # ---- U5 启动 updater 应用（经 SystemUI -> sandbox-run 的真实启动路径） ----
    # 前置：开机第一屏是锁屏，任何 tap 解锁（SystemUI tap 处理：locked -> 任意点击解锁）
    qmp = QMP(args.log + ".qmp")
    if not qmp.handshake():
        record("U5 QMP 握手", False, "handshake failed")
        return
    record("U5 QMP 握手", True, "")
    ux, uy = screen_to_tablet(args.xres // 2, args.yres // 2, args.xres, args.yres)
    ok = qmp.send_pointer(ux, uy)
    record("U5b 锁屏 tap 解锁", ok, "")
    found, data = con.wait("systemui: tap -> unlock", 15)
    record("U5c 串口确认 unlock", found, "" if found else tail(data))

    mark = con.mark()
    con.send("aether-sysuictl -c 'launch aetheros.updater'; echo LAUNCH_DONE")
    found, data = con.wait_line("LAUNCH_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    record("U5d sysuictl launch aetheros.updater", found and "\"ok\":true" in seg,
           "" if "\"ok\":true" in seg else tail(seg))
    if not found:
        return

    # ---- U6 窗口注册 + sock= 登记（compositor list 可观测） ----
    mark = con.mark()
    con.send("aethercompctl -c list; echo LIST_DONE")
    found, data = con.wait_line("LIST_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    has_win = "aether-updater" in seg
    has_sock = "/tmp/aether-updater.sock" in seg
    record("U6 updater 窗口已注册", found and has_win, "" if has_win else tail(seg))
    record("U6b 窗口 list 上报 sock=/tmp/aether-updater.sock",
           found and has_sock, "" if has_sock else tail(seg))
    if not (has_win and has_sock):
        return

    # 客机侧自报行（updater stderr -> 串口）
    found, _ = con.wait("updater: window id=", 10)
    record("U6c 客机串口出现 updater 自报行", found, "")

    # ---- U7 tap「检查更新」-> 宿主 GET /api/update-check ----
    x, y = screen_to_tablet(CONTENT_DX + BTN_CHECK[0], CONTENT_DY + BTN_CHECK[1],
                            args.xres, args.yres)
    ok = qmp.send_pointer(x, y)
    record("U7b tap 注入（检查更新按钮）", ok, "")

    deadline = time.time() + 20
    hit = False
    while time.time() < deadline:
        if any(h == ("GET", "/api/update-check") for h in MOCK_HITS):
            hit = True
            break
        time.sleep(0.5)
    record("U7c 宿主收到 GET /api/update-check", hit,
           "" if hit else f"mock hits: {MOCK_HITS}")

    # 等 updater 拿到应答进入 HAS_UPDATE（串口可见检查过程由 UI 状态体现）
    found, data = con.wait("aethercomp: tap -> aether-updater", 15)
    record("U7d compositor 转发 tap 到 updater 窗口", found,
           "" if found else "串口未见 tap 转发行")

    # ---- U8 tap「立即更新」-> 宿主 POST /api/update ----
    x, y = screen_to_tablet(CONTENT_DX + BTN_UPDATE[0], CONTENT_DY + BTN_UPDATE[1],
                            args.xres, args.yres)
    ok = qmp.send_pointer(x, y)
    record("U8 tap 注入（立即更新按钮）", ok, "")

    deadline = time.time() + 20
    hit = False
    while time.time() < deadline:
        if any(h == ("POST", "/api/update") for h in MOCK_HITS):
            hit = True
            break
        time.sleep(0.5)
    record("U8b 宿主收到 POST /api/update", hit,
           "" if hit else f"mock hits: {MOCK_HITS}")

    # ---- U9 updater 状态机进入 updating（客机串口/UI 文案） ----
    found, data = con.wait("aethercomp: tap -> aether-updater", 15, since=len(data))
    record("U9 第二次 tap 也到达 updater 窗口", found, "")


if __name__ == "__main__":
    sys.exit(main())
