#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AetherOS Phone App — 本地桥接服务（纯标准库，适配 Termux）。

职责：
  1. 托管前端 App（终端面板 + 内嵌 VNC 显示器）。
  2. /api/install  下载(镜像兜底) + 解包 + 启动 QEMU（VNC 在 127.0.0.1:5900）。
  3. /api/status   返回安装/启动状态 JSON。
  4. /api/logs     SSE 实时推送安装 + 客机串口日志。
  5. /ws/vnc       把浏览器 WebSocket 桥接到 QEMU 的 VNC TCP（websockify）。

无第三方依赖，Termux 自带 python3 即可运行。
"""

import base64
import hashlib
import http.server
import json
import os
import shutil
import socketserver
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request

# ---------------------------------------------------------------------------
# 路径与配置
# ---------------------------------------------------------------------------
APP_DIR = os.path.dirname(os.path.abspath(__file__))
WORK_DIR = os.environ.get("AETHER_APP_DIR", os.path.expanduser("~/aetheros-app"))
SYSTEM_DIR = os.path.join(WORK_DIR, "system")
LOG_FILE = os.path.join(WORK_DIR, "aetheros-app.log")
SERIAL_LOG = os.path.join(WORK_DIR, "serial.log")

QEMU_MEM = os.environ.get("AETHER_MEM", "512M")
QEMU_SMP = os.environ.get("AETHER_SMP", "2")
QEMU_RES = os.environ.get("AETHER_RES", "720x1600")
QEMU_VNC = os.environ.get("AETHER_VNC", "127.0.0.1:0")
HTTP_PORT = int(os.environ.get("AETHER_HTTP_PORT", "8080"))

REPO = "sitong-zhang/AetherOS-Phone"
TAG = "phone-v6"  # 永远拉最新：脚本也支持 ? 覆盖，这里用 latest 语义的 tag
MIRRORS = [
    "https://gh-proxy.com/https://github.com",
    "https://ghfast.top/https://github.com",
    "https://github.com",
]
SYSTEM_TARBALL = "aetheros-phone.tar.gz"

# 运行状态（线程安全用 _lock 保护）
_lock = threading.Lock()
_state = {
    "installing": False,
    "installed": False,
    "qemu_pid": None,
    "version": TAG,
}

_log_fh = None


def log(msg):
    """写一行日志到文件 + 控制台。"""
    line = "[aether] %s" % msg
    ts = time.strftime("%H:%M:%S")
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write("%s %s\n" % (ts, line))
    except Exception:
        pass
    print("%s %s" % (ts, line), flush=True)


# ---------------------------------------------------------------------------
# 下载（镜像自动兜底）
# ---------------------------------------------------------------------------
def _download(url, dest, timeout=60):
    """url 形如 https://github.com/...；逐镜像把前缀 https://github.com 换成代理基址。"""
    last_err = ""
    for base in MIRRORS:
        full = url.replace("https://github.com", base.rstrip("/"), 1)
        try:
            log("下载通道尝试: %s" % full)
            req = urllib.request.Request(full, headers={"User-Agent": "AetherOS-Phone-App"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                data = r.read()
            if len(data) < 1024 * 1024:
                raise IOError("文件过小(%d字节)，疑似错误页" % len(data))
            with open(dest, "wb") as f:
                f.write(data)
            log("下载完成: %s (%d 字节)" % (os.path.basename(dest), len(data)))
            return True
        except Exception as e:
            last_err = str(e)
            log("通道不可用: %s -> %s" % (base, e))
            continue
    raise IOError("所有镜像均不可用: %s" % last_err)


# ---------------------------------------------------------------------------
# 安装（下载 + 解包 + 启动 QEMU）
# ---------------------------------------------------------------------------
def _system_present():
    k = os.path.join(SYSTEM_DIR, "aetheros-kernel-aarch64.Image")
    r = os.path.join(SYSTEM_DIR, "aetheros-rootfs.ext4")
    return os.path.isfile(k) and os.path.isfile(r)


def _qemu_alive():
    pid = _state.get("qemu_pid")
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _vnc_target():
    """解析 QEMU_VNC -> (host, port)。"""
    host, port = QEMU_VNC.split(":")
    return host, 5900 + int(port)


def _vnc_alive():
    """探测 VNC 端口是否在监听（服务重启后 QEMU 仍活着时的兜底判定）。"""
    try:
        host, port = _vnc_target()
        with socket.create_connection((host, port), timeout=1):
            return True
    except Exception:
        return False


def _launch_qemu():
    if _qemu_alive():
        return True
    if _vnc_alive():
        log("检测到 VNC 已在监听（QEMU 应由先前实例启动），复用")
        return True
    if not _system_present():
        return False
    qemu = shutil.which("qemu-system-aarch64")
    if not qemu:
        log("未找到 qemu-system-aarch64，请先在 Termux 执行: pkg install qemu-system-aarch64-headless")
        return False
    rx, ry = QEMU_RES.split("x") if "x" in QEMU_RES else ("720", "1600")
    kernel = os.path.join(SYSTEM_DIR, "aetheros-kernel-aarch64.Image")
    rootfs = os.path.join(SYSTEM_DIR, "aetheros-rootfs.ext4")
    args = [
        qemu, "-M", "virt", "-cpu", "cortex-a57",
        "-smp", QEMU_SMP, "-m", QEMU_MEM,
        "-kernel", kernel,
        "-drive", "if=virtio,format=raw,file=%s" % rootfs,
        "-append", "console=ttyAMA0 root=/dev/vda rw ip=dhcp",
        "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
        "-device", "virtio-gpu-pci,xres=%s,yres=%s" % (rx, ry),
        "-device", "virtio-keyboard-pci",
        "-device", "virtio-tablet-pci",
        "-vnc", QEMU_VNC,
        "-snapshot",
        "-serial", "file:%s" % SERIAL_LOG,
        "-display", "none",
        "-monitor", "none",
    ]
    log("启动 QEMU: %s (VNC %s, %sx%s)" % (os.path.basename(qemu), QEMU_VNC, rx, ry))
    try:
        p = subprocess.Popen(args, cwd=SYSTEM_DIR, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        _state["qemu_pid"] = p.pid
        log("QEMU 已启动 pid=%d" % p.pid)
        return True
    except Exception as e:
        log("QEMU 启动失败: %s" % e)
        return False


def install_thread():
    with _lock:
        if _state["installing"]:
            return
        _state["installing"] = True
    try:
        os.makedirs(WORK_DIR, exist_ok=True)
        os.makedirs(SYSTEM_DIR, exist_ok=True)
        if not _system_present():
            tb = os.path.join(WORK_DIR, SYSTEM_TARBALL)
            log("开始下载系统镜像(优先最新版) ...")
            url_latest = "https://github.com/%s/releases/latest/download/%s" % (REPO, SYSTEM_TARBALL)
            url_tag = "https://github.com/%s/releases/download/%s/%s" % (REPO, TAG, SYSTEM_TARBALL)
            try:
                try:
                    _download(url_latest, tb, timeout=180)
                    with _lock:
                        _state["version"] = "latest"
                except Exception:
                    log("latest 通道失败，兜底到 %s ..." % TAG)
                    _download(url_tag, tb, timeout=180)
            except Exception as e:
                log("下载失败: %s" % e)
                with _lock:
                    _state["installing"] = False
                return
            log("解包系统镜像...")
            with tarfile.open(tb, "r:gz") as tf:
                tf.extractall(SYSTEM_DIR)
            os.remove(tb)
            log("解包完成")
            with _lock:
                _state["installed"] = True
        else:
            log("系统已存在，跳过下载")
            with _lock:
                _state["installed"] = True
        _launch_qemu()
    except Exception as e:
        log("安装异常: %s" % e)
    finally:
        with _lock:
            _state["installing"] = False


def start_install():
    with _lock:
        if _state["installing"]:
            return False
        if _state["installed"] and _qemu_alive():
            return False
    t = threading.Thread(target=install_thread, daemon=True)
    t.start()
    return True


# ---------------------------------------------------------------------------
# WebSocket（手动实现 RFC6455，零依赖）→ VNC TCP 桥接
# ---------------------------------------------------------------------------
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _read_exact(stream, n):
    """从 socket(recv) 或文件流(read) 精确读取 n 字节。"""
    buf = b""
    while len(buf) < n:
        if hasattr(stream, "read"):
            chunk = stream.read(n - len(buf))
        else:
            chunk = stream.recv(n - len(buf))
        if not chunk:
            raise IOError("连接关闭")
        buf += chunk
    return buf


def _ws_encode(opcode, payload):
    b = bytearray()
    b.append(0x80 | opcode)
    ln = len(payload)
    if ln < 126:
        b.append(ln)
    elif ln < 65536:
        b.append(126)
        b += struct.pack(">H", ln)
    else:
        b.append(127)
        b += struct.pack(">Q", ln)
    b += payload
    return bytes(b)


def _ws_recv_frame(stream):
    head = _read_exact(stream, 2)
    fin = (head[0] & 0x80) != 0
    opcode = head[0] & 0x0F
    masked = (head[1] & 0x80) != 0
    ln = head[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", _read_exact(stream, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", _read_exact(stream, 8))[0]
    key = b""
    if masked:
        key = _read_exact(stream, 4)
    data = _read_exact(stream, ln)
    if masked:
        data = bytes(data[i] ^ key[i % 4] for i in range(len(data)))
    return fin, opcode, data


def handle_websocket(handler, vnc_host, vnc_port):
    """WS↔VNC 桥接。复用 http.server 已解析完握手的 rfile/wfile 流，
    避免重复读取导致阻塞；握手头已由 do_GET 前的请求解析消费。"""
    try:
        rfile = handler.rfile
        wfile = handler.wfile
        accept = base64.b64encode(
            hashlib.sha1((handler.headers.get("Sec-WebSocket-Key", "") + WS_GUID).encode()).digest()
        ).decode()
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n" % accept
        )
        wfile.write(resp.encode())
        wfile.flush()

        import socket
        vsock = socket.create_connection((vnc_host, vnc_port), timeout=5)
        vsock.settimeout(None)  # 阻塞读取，避免空闲时线程自杀
        log("bridge: 已连接 VNC %s:%s" % (vnc_host, vnc_port))

        def client_to_vnc():
            try:
                while True:
                    _, opcode, data = _ws_recv_frame(rfile)
                    if opcode == 0x8:  # close
                        break
                    if opcode == 0x9:  # ping
                        wfile.write(_ws_encode(0xA, data)); wfile.flush()
                        continue
                    if opcode in (0x1, 0x2):
                        vsock.sendall(data)
                        if len(data) < 200:
                            log("bridge c->v: %d 字节 (op=%d) %s" % (len(data), opcode, data.hex()))
            except Exception as e:
                log("bridge client->vnc 异常: %s" % e)
            finally:
                try:
                    vsock.close()
                except Exception:
                    pass

        def vnc_to_client():
            try:
                total = 0
                while True:
                    data = vsock.recv(65536)
                    if not data:
                        log("bridge vnc->client: VNC 连接 EOF(关闭)")
                        break
                    total += len(data)
                    log("bridge v->c: %d 字节 (累计 %d)" % (len(data), total))
                    wfile.write(_ws_encode(0x2, data)); wfile.flush()
            except Exception as e:
                log("bridge vnc->client 异常: %s" % e)
            finally:
                try:
                    wfile.write(_ws_encode(0x8, b"")); wfile.flush()
                except Exception:
                    pass

        t1 = threading.Thread(target=client_to_vnc, daemon=True)
        t2 = threading.Thread(target=vnc_to_client, daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()
    except Exception as e:
        log("WS 桥接异常: %s" % e)


# ---------------------------------------------------------------------------
# HTTP 处理
# ---------------------------------------------------------------------------
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/" or path == "/index.html":
            self._send_file("index.html", "text/html; charset=utf-8")
        elif path == "/api/status":
            self._send_status()
        elif path == "/api/logs":
            self._sse_logs()
        elif path == "/ws/vnc" or path.startswith("/ws/vnc"):
            # 接管连接做 WebSocket（复用 rfile/wfile 流）
            try:
                host, port = _vnc_target()
                handle_websocket(self, host, port)
            except Exception as e:
                log("WS 桥接失败: %s" % e)
        else:
            # 静态文件（白名单扩展名）
            name = os.path.basename(path)
            ctype = {
                ".js": "application/javascript; charset=utf-8",
                ".webmanifest": "application/manifest+json",
                ".png": "image/png",
                ".css": "text/css; charset=utf-8",
                ".html": "text/html; charset=utf-8",
            }.get(os.path.splitext(name)[1], "application/octet-stream")
            self._send_file(name, ctype)

    def _send_file(self, name, ctype):
        p = os.path.join(APP_DIR, name)
        if not os.path.isfile(p):
            self._send(404, "missing %s" % name)
            return
        with open(p, "rb") as f:
            self._send(200, f.read(), ctype)

    def _send_status(self):
        with _lock:
            installed = _state["installed"] or _system_present()
            booted = _qemu_alive()
        if installed and not booted:
            booted = _vnc_alive()
        self._send(200, json.dumps({
            "installed": installed,
            "booted": booted,
            "installing": _state["installing"],
            "version": _state.get("version") or TAG,
            "vnc": QEMU_VNC,
            "port": HTTP_PORT,
        }), "application/json")

    def _sse_logs(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        # 先发历史
        try:
            if os.path.isfile(LOG_FILE):
                with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
                    for line in f:
                        self.wfile.write(("data: %s\n" % line.rstrip("\n")).encode())
                    self.wfile.write(b"\n")
                    self.wfile.flush()
            if os.path.isfile(SERIAL_LOG):
                with open(SERIAL_LOG, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(0, os.SEEK_END)
        except Exception:
            pass
        last_log = os.path.getsize(LOG_FILE) if os.path.isfile(LOG_FILE) else 0
        last_serial = os.path.getsize(SERIAL_LOG) if os.path.isfile(SERIAL_LOG) else 0
        try:
            while True:
                time.sleep(0.4)
                if os.path.isfile(LOG_FILE):
                    sz = os.path.getsize(LOG_FILE)
                    if sz > last_log:
                        with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
                            f.seek(last_log)
                            chunk = f.read(sz - last_log)
                        for line in chunk.split("\n"):
                            if line:
                                self.wfile.write(("data: %s\n" % line).encode())
                        self.wfile.write(b"\n")
                        self.wfile.flush()
                        last_log = sz
                if os.path.isfile(SERIAL_LOG):
                    sz = os.path.getsize(SERIAL_LOG)
                    if sz > last_serial:
                        with open(SERIAL_LOG, "r", encoding="utf-8", errors="replace") as f:
                            f.seek(last_serial)
                            chunk = f.read(sz - last_serial)
                        for line in chunk.split("\n"):
                            if line:
                                self.wfile.write(("data: %s\n" % line).encode())
                        self.wfile.write(b"\n")
                        self.wfile.flush()
                        last_serial = sz
        except Exception:
            pass

    def do_POST(self):
        path = self.path.split("?")[0]
        if path == "/api/install":
            ok = start_install()
            self._send(200, json.dumps({"started": ok}), "application/json")
        else:
            self._send(404, "not found")


def main():
    os.makedirs(WORK_DIR, exist_ok=True)
    # 清空旧日志，避免首屏刷一大堆历史
    if os.path.isfile(LOG_FILE):
        try:
            os.remove(LOG_FILE)
        except Exception:
            pass
    log("AetherOS Phone App 本地服务启动 (端口 %d)" % HTTP_PORT)
    log("QEMU VNC 目标: %s" % QEMU_VNC)
    # 若系统已存在则尝试直接拉起 QEMU（断点续跑）
    if _system_present():
        with _lock:
            _state["installed"] = True
        threading.Thread(target=_launch_qemu, daemon=True).start()
    socketserver.TCPServer.allow_reuse_address = True
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("服务停止")


if __name__ == "__main__":
    main()
