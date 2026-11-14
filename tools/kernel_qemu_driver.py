#!/usr/bin/env python3
"""
AetherOS 内核 · QEMU TCG 真实启动驱动（自动测试 + 取证）

职责：
  1. 自己在 unix socket 上 listen，再让 QEMU 作为 chardev 客户端连进来
     （保证内核最早期的串口输出不丢），双向读写 = 真实串口输入/输出链路。
  2. QMP 连接，用 screendump 导出 **QEMU 真实 VGA 帧缓冲** PPM -> PNG 自查。
  3. 解析串口结构化日志，断言完整引导链 + 安全演示闭环。
  4. 注入交互命令（status/selftest/exit）验证 ring3 输入链路真的生效。
  5. 收集 isa-debug-exit 退出码，产出证据四件套。

不伪造任何日志；所有断言来自 QEMU 真实串口输出。
"""
import argparse
import json
import os
import re
import select
import socket
import subprocess
import sys
import time
from datetime import datetime

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "kernel")
EVID = os.path.join(ROOT, "evidence", "round-003")
SER_SOCK = os.path.join(OUT, "aether-serial.sock")
QMP_SOCK = os.path.join(OUT, "aether-qmp.sock")

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
KERNEL = os.path.join(OUT, "aether-kernel.boot")
DISK = os.path.join(OUT, "aether-data.img")

# 自动演示必须出现的标记（机器可判，来自内核/用户态真实输出）
REQUIRED = [
    "POST PASS  multiboot handoff",
    "POST PASS  IA-32e 64-bit long mode",
    "POST PASS  physical memory manager",
    "POST PASS  virtual memory manager",
    "POST PASS  GDT / TSS / IDT / 8259 PIC",
    "POST PASS  ATA PIO block device",
    "POST PASS  SHA-256 standard vector",
    "POST PASS  timer tick advanced",
    "POST PASS  no CPU exception during boot",
    "POST PASS  embedded initrd",
    "ELF OK:",
    "AETHER_RING3",  # 见下方兜底，实际由 BOOT_MAGIC_OK 系列代表
]
# 上面 AETHER_RING3 是占位，用真实标记替换
REQUIRED = [x for x in REQUIRED if x != "AETHER_RING3"] + [
    "=== AetherOS ring3 init :: SECURITY DEMO ===",
    "BOOT_MAGIC_OK",
    "SELFTEST crypto_sha256=PASS",
    "PROVISION_OK",
    "UNLOCK_OK",
    "VAULT_PUT_OK x3",
    "VAULT_GET_OK",
    "LOCK_OK",
    "DENIED: wrong password (1/3)",
    "DENIED: wrong password (2/3)",
    "WIPED: 3 wrong passwords reached threshold 3",
    "WIPE_RECEIPT_OK sys_unchanged=true",
    "REINIT_OK",
    "OLD_DATA_GONE",
    "DEMO_DONE=1  ALL STAGES PASSED",
    "SHELL READY",
    "KERNEL_BOOT_FAILS=0 SELFTEST_DONE=1",
    "PROC: created 2 ring3 task(s)",
    "TASK2 PID=2 tick=",
    "TASK2 DONE",
]

# 交互注入命令 -> 期望回显（证明输入链路 + ring3 再次进 ring0）
INJECT = [
    ("status\n", "SHELL CMD: status"),
    ("selftest\n", "SHELL CMD: selftest"),
    ("exit\n", "SHELL_EXIT"),
]


class QMPSession:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError, OSError):
                time.sleep(0.1)
        else:
            raise RuntimeError("QMP socket 连接失败")
        self.sock.settimeout(5)
        self.f = self.sock.makefile("rwb", buffering=0)
        self.f.readline()                      # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, args=None):
        self.f.write((json.dumps({"execute": name, "arguments": args or {}}) + "\r\n").encode())
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP 连接关闭")
            obj = json.loads(line)
            if "return" in obj or "error" in obj:
                return obj
            # 事件（如 SHUTDOWN）跳过

    def screendump(self, ppm_path):
        return self.cmd("screendump", {"filename": ppm_path})

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass


def ppm_to_png(ppm, png):
    if not os.path.exists(ppm) or os.path.getsize(ppm) == 0:
        return False
    with open(ppm, "rb") as f:
        data = f.read()
    # 解析 P6：magic, 宽, 高, maxval，可能含注释
    idx = 0
    tokens = []
    while len(tokens) < 4:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx:idx + 1] not in (b"\n", b"\r"):
                idx += 1
            continue
        s = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        tokens.append(data[s:idx])
        idx += 1
    magic, w, h, maxv = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
    if magic != b"P6":
        return False
    raw = data[idx:]
    img = Image.frombytes("RGB", (w, h), raw)
    img.save(png)
    return True


def start_qemu():
    for p in (SER_SOCK, QMP_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    # Python 先 listen，QEMU 作为客户端连入 -> 早期串口日志不丢
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(SER_SOCK)
    listener.listen(1)

    cmd = [
        QEMU,
        "-machine", "pc,accel=tcg",
        "-cpu", "max",
        "-m", "512M",
        "-kernel", KERNEL,
        "-drive", f"file={DISK},format=raw,if=ide,media=disk",
        "-chardev", f"socket,id=s0,path={SER_SOCK},server=off",
        "-serial", "chardev:s0",
        "-qmp", f"unix:{QMP_SOCK},server=on,wait=off",
        "-display", "none",
        "-no-reboot", "-no-shutdown",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    env = dict(os.environ)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

    listener.settimeout(30)
    try:
        conn, _ = listener.accept()
    except socket.timeout:
        proc.kill()
        raise RuntimeError("QEMU 未连接串口 socket")
    conn.settimeout(0.2)
    qmp = QMPSession(QMP_SOCK)
    return proc, conn, qmp, listener, cmd


def read_until(conn, log, markers, timeout, on_line=None):
    """读串口字节，逐行累积，直到 markers 全部出现或超时。返回日志全文。"""
    deadline = time.time() + timeout
    buf = b""
    seen = set()
    while time.time() < deadline:
        r, _, _ = select.select([conn], [], [], 0.2)
        if r:
            try:
                chunk = conn.recv(65536)
            except (BlockingIOError, socket.timeout):
                chunk = b""
            if not chunk:
                break
            buf += chunk
            log += chunk.decode("utf-8", "replace")
            if on_line:
                on_line(log)
        for m in markers:
            if m in log:
                seen.add(m)
        if len(seen) == len(markers):
            break
    return log


def run(mode="test"):
    os.makedirs(EVID, exist_ok=True)
    if not os.path.isfile(KERNEL):
        print("缺少内核镜像，请先 scripts/kernel.sh build", file=sys.stderr)
        return 2
    os.makedirs(OUT, exist_ok=True)
    # 每次运行重置数据盘为全零，保证可复现（出厂态）
    sectors = 4096
    with open(os.path.join(ROOT, "kernel/include/aether_kconfig.h"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*#define\s+K_DISK_TOTAL_SECTORS\s+(\d+)", line)
            if m:
                sectors = int(m.group(1))
                break
    with open(DISK, "wb") as f:
        f.truncate(sectors * 512)

    print("=== AetherOS 内核 QEMU TCG 真实启动 ===")
    proc, conn, qmp, listener, qemucmd = start_qemu()
    log = ""
    shots = []
    done = {"post": False, "demo": False, "shell": False}
    ser_live = os.path.join(EVID, "serial-live.log")
    with open(ser_live, "w", encoding="utf-8") as _f:
        _f.write("")
    _seen = [0]

    def maybe_shoot(text):
        # 按真实日志阶段触发 QMP screendump（真实 VGA 帧缓冲）。
        # 关键：在 marker 出现的「瞬间」就抓（此时 QEMU 还活着、内核刚打印该标记），
        # 不能在 finalize 之后统一抓——那时内核已发 isa-debug-exit、QEMU 正在退出，
        # screendump 必失败。
        def shoot(name):
            ppm = os.path.join(EVID, name + ".ppm")
            png = os.path.join(EVID, name + ".png")
            try:
                qmp.screendump(ppm)
                if ppm_to_png(ppm, png):
                    shots.append(png)
                    print(f"  截图 {name} OK")
            except Exception as e:
                print(f"  截图 {name} 失败（如实记录）: {e}")
        if not done["post"] and "boot self-check summary" in text:
            done["post"] = True
            shoot("01-boot-post")
        if not done["demo"] and "DEMO_DONE=1" in text:
            done["demo"] = True
            shoot("02-data-destruction")
        if not done["shell"] and "SHELL READY" in text:
            done["shell"] = True
            shoot("03-interactive-shell")

    def on_line(text):
        # 实时落盘：即使被 timeout kill，也能拿到真实串口内容用于定位
        maybe_shoot(text)
        chunk = text[_seen[0]:]
        _seen[0] = len(text)
        if chunk:
            with open(ser_live, "a", encoding="utf-8") as _f:
                _f.write(chunk)

    t0 = time.time()
    # 阶段 1：等到 SHELL READY（自动演示跑完）
    log = read_until(conn, log, ["SHELL READY"], timeout=150, on_line=on_line)
    # 阶段 2：注入交互命令
    injected_log = ""
    for cmd, expect in INJECT:
        conn.sendall(cmd.encode())
        time.sleep(0.35)
        r, _, _ = select.select([conn], [], [], 1.0)
        if r:
            try:
                injected_log += conn.recv(65536).decode("utf-8", "replace")
            except (BlockingIOError, socket.timeout):
                pass
    log += injected_log
    # 阶段 3：等到内核 finalize（exit 触发 isa-debug-exit）
    log = read_until(conn, log, ["KERNEL_EXIT_CODE"], timeout=30)

    # 截图已在各阶段 marker 出现的瞬间实时抓取（见 maybe_shoot）；此处不再重复。
    pngs = shots

    # 等 QEMU 退出码
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        qmp.quit()
        proc.wait(timeout=10)
    qmp.quit()
    conn.close()
    listener.close()
    rc = proc.returncode

    # 保存原始日志
    serial_log = os.path.join(EVID, "qemu-serial.log")
    with open(serial_log, "w", encoding="utf-8") as f:
        f.write(log)
    with open(os.path.join(EVID, "qemu-cmdline.txt"), "w", encoding="utf-8") as f:
        f.write(" ".join(qemucmd) + "\n")
    with open(os.path.join(EVID, "qemu-exit-code.txt"), "w", encoding="utf-8") as f:
        f.write(str(rc) + "\n")

    # ---------- 断言 ----------
    results = []
    for m in REQUIRED:
        results.append((m, m in log))
    for cmd, expect in INJECT:
        results.append((f"交互注入 {cmd.strip()} -> {expect}", expect in log))
    results.append(("QEMU 正常退出(isa-debug-exit 33)", rc == 33))
    results.append(("无内核异常 / FAIL 标记", "[KERNEL FAULT]" not in log and "POST FAIL" not in log))
    results.append(("截图来自 QEMU 真实帧缓冲", len(pngs) > 0))

    passed = sum(1 for _, ok in results if ok)
    failed = len(results) - passed
    print("\n=== 断言结果 ===")
    for m, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {m}")
    print(f"\n合计：{passed} PASS / {failed} FAIL，QEMU 退出码={rc}，耗时 {time.time()-t0:.1f}s")

    summary = {
        "timestamp": datetime.now().isoformat(),
        "mode": mode,
        "qemu_exit_code": rc,
        "passed": passed,
        "failed": failed,
        "results": [{"assert": m, "ok": ok} for m, ok in results],
        "screenshots": [os.path.basename(p) for p in pngs],
        "qemu_cmdline": " ".join(qemucmd),
    }
    with open(os.path.join(EVID, "kernel-test-results.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    if failed:
        # 如实输出失败片段，便于定位
        print("\n--- 串口日志尾部（失败排查）---")
        print("\n".join(log.splitlines()[-40:]))
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["test", "evidence", "run"], default="test")
    a = ap.parse_args()
    return run(a.mode)


if __name__ == "__main__":
    sys.exit(main())
