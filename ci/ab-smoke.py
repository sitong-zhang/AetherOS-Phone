#!/usr/bin/env python3
"""ab-smoke.py —— AetherOS A/B 双槽 OTA 冒烟（P1-2c，严格断言链）

对「真 A/B 双槽 + 持久分区 + initramfs 启动选择器 + aether-ota 槽位管理」
做端到端验证。所有期望字面值均取自本仓真实实现（ci/ab-initramfs-init.sh、
aether-ota.c、aether-init.c 的信号语义），不是想象中的输出。

断言链（任一失败即整体失败，退出码 1）：
  A. 首启 slot A : initramfs 选择器 banner(booting slot A /dev/vda)
                  -> 登录 -> /etc/aether-slot=A -> aether-ota --current=A
                  -> /proc/mounts 根文件系统确为 /dev/vda
  B. 拒绝坏槽   : QMP device_del 摘除 B 盘 -> aether-ota --switch 必须拒绝
                  (rc!=0 + "refusing to switch") 且槽位标记不变（no-brick 保证）
                  -> QMP device_add 恢复 B 盘 -> 客机 /dev/vdb 重现
  C. 切槽实启   : aether-ota --switch -> switched_slot=B
                  -> init 优雅重启(=== shutdown: reboot === + RB_AUTOBOOT)
                  -> 选择器 banner(booting slot B /dev/vdb) -> 登录 ->
                  /etc/aether-slot=B -> --current=B -> 根文件系统为 /dev/vdb
                  -> PID1 comm=aether-init（B 槽原生 init 接管）
  D. 恢复模式   : aether-ota --mark-recovery -> recovery_marked=1 + 优雅重启
                  -> AETHEROS_RECOVERY_MODE banner + apply validated=1
                  recovery_cleared=1 -> 重启后仍回到 slot B（标志已清除）

纪律：不猜输出、不放宽断言、失败时打印日志尾部便于定位。
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time


# ---------------------------------------------------------------- 检查记录

class Report:
    def __init__(self):
        self.checks = []  # (name, ok, detail)

    def record(self, name, ok, detail=""):
        self.checks.append((name, ok, detail))
        mark = "PASS" if ok else "FAIL"
        line = f"[{mark}] {name}"
        if detail:
            line += f" —— {detail}"
        print(line, flush=True)

    @property
    def failed(self):
        return [c for c in self.checks if not c[1]]

    def summary_text(self):
        lines = ["AetherOS A/B OTA smoke summary", "=" * 40]
        for name, ok, detail in self.checks:
            lines.append(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" —— {detail}" if detail else ""))
        n_fail = len(self.failed)
        lines.append("=" * 40)
        lines.append(f"total={len(self.checks)} failed={n_fail} result={'OK' if n_fail == 0 else 'FAILED'}")
        return "\n".join(lines) + "\n"


REPORT = Report()


def tail(data, since=0, n=900):
    seg = data[since:] if since else data
    return seg[-n:].replace("\n", " | ")[-n:]


# ---------------------------------------------------------------- 控制台

class GuestConsole:
    """经 FIFO 驱动 QEMU 串口；日志落盘；所有等待都带截止时间。"""

    def __init__(self, log_path):
        self.log_path = log_path
        self.fifo_path = log_path + ".console.in"
        try:
            os.unlink(self.fifo_path)
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

    def mark(self):
        return len(self.read_log())

    @staticmethod
    def last_group(pattern, text, since=0):
        m = None
        for m in re.finditer(pattern, text[since:], re.MULTILINE):
            pass
        return m.group(1) if m else None


# ---------------------------------------------------------------- QMP

class QMP:
    """QEMU Machine Protocol 控制通道（unix socket）。

    用于 A/B 冒烟的负向测试：device_del 摘除 B 盘（模拟坏 OTA 目标），
    device_add 恢复。握手后所有 execute 方法均等待 return/error。"""

    def __init__(self, sock_path):
        import socket as _sock
        self.s = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
        for _ in range(50):
            try:
                self.s.connect(sock_path)
                break
            except OSError:
                time.sleep(0.2)
        self.s.settimeout(30)  # 任何 readline 不得无限阻塞（否则冒烟会挂死）
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
        # 1) 读 QMP 欢迎消息 {"QMP": {...}}（唯一一条非请求消息）
        m = self._read_msg()
        if m is None or "QMP" not in m:
            return False
        # 2) 发送 capabilities 协商
        self.f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
        self.f.flush()
        # 3) 等待协商结果
        while True:
            m = self._read_msg()
            if m is None:
                return False
            if "return" in m:
                return True
            if "error" in m:
                return False

    def execute(self, cmd, args=None):
        """通用 execute；返回 (ok, message)。"""
        payload = {"execute": cmd}
        if args:
            payload["arguments"] = args
        self.f.write((json.dumps(payload) + "\n").encode())
        self.f.flush()
        while True:
            m = self._read_msg()
            if m is None:
                return False, "connection closed"
            if "return" in m:
                return True, json.dumps(m["return"])
            if "error" in m:
                return False, m["error"].get("desc", "unknown error")

    def device_del(self, dev_id):
        return self.execute("device_del", {"id": dev_id})

    def drive_add(self, drive_id, path):
        """QMP 无 drive_add，经 human-monitor-command 走 HMP。
        PCIe unplug 真正完成时 QEMU 会释放 if=none 的 block backend
        （"Property 'virtio-blk-device.drive' can't find value" 实锤），
        恢复 B 盘必须先加回 drive 再 device_add。"""
        return self.execute("human-monitor-command",
                            {"command-line":
                             f"drive_add auto file={path},if=none,id={drive_id},format=raw"})

    def device_add(self, dev_id, drive, bus="rpB"):
        """device_add 必须显式 bus=rpB：virtio-blk-pci 默认总线是 pcie.0，
        PCIe 根总线不支持热插拔（'Bus pcie.0 does not support hotpluging'）——
        B 盘启动时挂在 pcie-root-port(id=rpB) 上，恢复时也要挂回去。"""
        return self.execute("device_add",
                            {"driver": "virtio-blk-pci", "drive": drive,
                             "id": dev_id, "bus": bus})


# ---------------------------------------------------------------- QEMU

def start_qemu(args, con):
    # 三盘：A 槽 / B 槽 / 持久分区，按 -device 顺序枚举为 vda/vdb/vdc。
    # if=none + 显式 device 使 QMP 可热插拔（负向测试摘除 B 盘）。
    cmd = [
        args.qemu, "-M", "virt", "-cpu", "cortex-a57", "-smp", "2", "-m", args.memory,
        "-kernel", args.kernel,
        "-initrd", args.initrd,
        # 无 root= ：由 initramfs /init 选择器决定挂哪个槽（P1-2c 核心语义）
        "-append", "console=ttyAMA0",
        # 三盘各自挂 pcie-root-port：PCIe 原生热插拔（客机内核
        # CONFIG_HOTPLUG_PCI_PCIE=y，P1-2c B 段负向测试依赖它）。
        # 枚举顺序坑：root port 的 secondary bus 是 DFS 递归扫描的，
        # 若只把 B 挂 root port 而 P 留在 pcie.0，Linux 枚举顺序是
        # A(根)→P(根)→B(secondary)，vdb 会变成 persist 盘（B2 负向
        # 测试曾实锤：挂 vdb 成功却 no init——那是 64M persist 盘）。
        # 三盘全挂 root port 后 DFS 顺序 A→B→P，vda/vdb/vdc 语义归位。
        # slot=N 区分物理槽位号：/sys/bus/pci/slots/N（power 控制用）。
        "-device", "pcie-root-port,id=rpA,bus=pcie.0,slot=0,chassis=0",
        "-drive", f"if=none,id=diskA,format=raw,file={args.slot_a}",
        "-device", "virtio-blk-pci,drive=diskA,id=diskA,bus=rpA",
        "-device", "pcie-root-port,id=rpB,bus=pcie.0,slot=1,chassis=1",
        "-drive", f"if=none,id=diskB,format=raw,file={args.slot_b}",
        "-device", "virtio-blk-pci,drive=diskB,id=diskB,bus=rpB",
        "-device", "pcie-root-port,id=rpP,bus=pcie.0,slot=2,chassis=2",
        "-drive", f"if=none,id=diskP,format=raw,file={args.persist}",
        "-device", "virtio-blk-pci,drive=diskP,id=diskP,bus=rpP",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0",
        # 合成器与 boot-smoke 默认路径一致（软件光栅化），证明每槽完整系统可启
        "-device", "virtio-gpu-pci",
        f"-chardev", f"socket,path={args.log}.qmp,server=on,wait=off,id=qmp",
        "-mon", "chardev=qmp,mode=control",
        # -snapshot：同 boot-smoke——A/B 冒烟会在客机内真实翻转槽位/写
        # persist，覆盖层保进程内重启状态，退出即弃，槽镜像不被跑脏。
        "-snapshot",
        "-display", "none", "-serial", "stdio", "-monitor", "none",
    ]
    print("[qemu] " + " ".join(cmd), flush=True)
    return subprocess.Popen(cmd, stdin=con.fin, stdout=con._logf, stderr=con._logf)


def stop_qemu(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)


# ---------------------------------------------------------------- 客机操作助手

def login_once(tag, boot_timeout, cmd_timeout):
    """等待 login: 并以 root（空密码）登录；返回 (ok, data)。

    必须 since=mark：客机 reboot 后 wait 从累计日志里能匹配到上一轮
    系统的旧 login: 提示，抢跑导致 "root" 在 boot 早期被 tty 丢弃、
    后续输入被 getty 当用户名回显——C 段曾因此连环误判（B0b 把串口
    回显当成 shell 回显，PASS 后 C4 起全部 Login incorrect）。"""
    mark = con.mark()
    found, data = con.wait("login:", boot_timeout, since=mark)
    if not found:
        REPORT.record(f"{tag}0 getty 登录提示出现", False,
                      f"{boot_timeout}s 超时; 日志尾部: {tail(data, mark)}")
        return False, data
    REPORT.record(f"{tag}0 getty 登录提示出现", True)
    time.sleep(1.0)
    con.send("root")
    time.sleep(1.5)
    con.send("")  # root 空密码（/etc/shadow: root::）
    time.sleep(1.5)
    mark2 = con.mark()
    con.send(f"echo LOGIN_OK_{tag}")
    found, data = con.wait(f"LOGIN_OK_{tag}", cmd_timeout, since=mark2)
    # 防回显误判：shell 执行时 marker 单独成行（echo 的输出）；tty 回显时
    # marker 跟在 "aetheros login: " 前缀后同一行，不会独立成行。
    seg = data[mark2:]
    marker_alone = re.search(rf"^LOGIN_OK_{tag}\s*$", seg, re.M) is not None
    really_shell = marker_alone and ("Login incorrect" not in seg)
    REPORT.record(f"{tag}0b shell 回显 LOGIN_OK_{tag}（真 shell，非 tty 回显）",
                  found and really_shell,
                  "" if (found and really_shell) else
                  f"marker={found} 回显误判或登录失败; 尾部: {tail(seg)}")
    return found and really_shell, data


def wait_slot_banner(slot, timeout):
    """等待 initramfs 选择器 banner：aetheros-ab: booting slot X (/dev/vdY)。

    since=mark：C 段与 D 段都是 slot B banner，无 since 会把上一轮
    C 段的 banner 当成 D 段的（误 PASS）。"""
    dev = "/dev/vdb" if slot == "B" else "/dev/vda"
    marker = f"aetheros-ab: booting slot {slot} ({dev})"
    mark = con.mark()
    found, data = con.wait(marker, timeout, since=mark)
    REPORT.record(f"选择器 banner: booting slot {slot} ({dev})", found,
                  "" if found else f"{timeout}s 超时; 日志尾部: {tail(data, mark)}")
    return found, data


def guest_cmd(cmd, done_tag, cmd_timeout):
    """在客机执行命令并等到 done 标记；返回 (ok, seg)。"""
    mark = con.mark()
    con.send(f"{cmd}; echo {done_tag}")
    found, data = con.wait(done_tag, cmd_timeout, since=mark)
    return found, data[mark:]


# ---------------------------------------------------------------- 主流程

con = None


def main():
    global con
    ap = argparse.ArgumentParser(description="AetherOS A/B OTA smoke (strict assertion chain)")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True, help="aetheros-ab-initramfs.cpio.gz")
    ap.add_argument("--slot-a", required=True)
    ap.add_argument("--slot-b", required=True)
    ap.add_argument("--persist", required=True)
    ap.add_argument("--log", default="/tmp/aether-ab-smoke.log")
    ap.add_argument("--summary", default="/tmp/aether-ab-smoke.summary.txt")
    ap.add_argument("--qemu", default="qemu-system-aarch64")
    ap.add_argument("--memory", default="512M")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--cmd-timeout", type=int, default=90)
    args = ap.parse_args()

    for p in (args.kernel, args.initrd, args.slot_a, args.slot_b, args.persist):
        if not os.path.isfile(p):
            print(f"[FATAL] 输入不存在: {p}", file=sys.stderr)
            return 2

    con = GuestConsole(args.log)
    proc = start_qemu(args, con)
    try:
        try:
            run_checks(args, proc)
        except Exception as e:
            REPORT.record("X0 冒烟驱动未预期异常", False, f"{type(e).__name__}: {e}")
    finally:
        stop_qemu(proc)
        con._logf.close()
        os.close(con.fin)
        try:
            os.unlink(con.fifo_path)
        except FileNotFoundError:
            pass
        with open(args.summary, "w") as f:
            f.write(REPORT.summary_text())
        print(f"[ab-smoke] summary -> {args.summary}")
    return 0 if not REPORT.failed else 1


def run_checks(args, proc):
    qmp = QMP(args.log + ".qmp")
    if not qmp.handshake():
        REPORT.record("QMP 控制通道握手", False, "无法握手（负向测试与恢复流程依赖 QMP）")
        return
    REPORT.record("QMP 控制通道握手", True)

    # ---- A. 首启 slot A -------------------------------------------------
    print("[ab-smoke] ==== 阶段 A：首启 slot A ====", flush=True)
    ok, _ = wait_slot_banner("A", args.boot_timeout)
    if not ok:
        return
    ok, _ = login_once("A", args.boot_timeout, args.cmd_timeout)
    if not ok:
        return

    found, seg = guest_cmd("cat /etc/aether-slot", "SLOTMARK_DONE", args.cmd_timeout)
    slotmark = GuestConsole.last_group(r"^([AB])\s*$", seg)
    REPORT.record("A3 客机自报 /etc/aether-slot=A", bool(slotmark) and slotmark == "A",
                  f"marker={slotmark!r}" if slotmark else "未解析到槽位标记")

    found, seg = guest_cmd("aether-ota --current", "OTA_CUR_DONE", args.cmd_timeout)
    cur = GuestConsole.last_group(r"active_slot=([AB])", seg)
    REPORT.record("A4 aether-ota --current=A（持久分区标记与盘上标记一致）",
                  bool(cur) and cur == "A",
                  f"active_slot={cur}" if cur else "未解析到 active_slot")

    found, seg = guest_cmd("grep ' / ' /proc/mounts", "ROOTMNT_DONE", args.cmd_timeout)
    rootdev = GuestConsole.last_group(r"^(/dev/vd[ab])\s+/\s", seg)
    REPORT.record("A5 根文件系统确为 /dev/vda（选择器挂盘交棒的实证）",
                  rootdev == "/dev/vda",
                  f"root={rootdev}" if rootdev else f"未解析; 尾部: {tail(seg)}")

    # ---- B. 拒绝坏槽（no-brick 保证）--------------------------------------
    print("[ab-smoke] ==== 阶段 B：拒绝未校验的目标槽 ====", flush=True)
    ok, msg = qmp.device_del("diskB")
    REPORT.record("B1 QMP device_del 摘除 B 盘（模拟损坏的 OTA 目标）", ok,
                  msg if not ok else "diskB 摘除请求已被 QEMU 接受")
    if not ok:
        return
    # PCIe 原生热插拔是异步流程：QMP device_del 返回只代表请求被接受，
    # QEMU 拉低 present detect 后要等客机 pciehp ISR 检测到卡不在、
    # 自动 disable slot 并回写 SLOTCTRL，QEMU 才真正 unrealize 设备。
    # 因此必须轮询等 /dev/vdb 真正消失（devtmpfs 摘除），不能盲睡。
    gone = False
    for i in range(30):
        found, seg = guest_cmd("ls /dev/vdb", f"VDBG_DONE", args.cmd_timeout)
        if "No such file" in seg or "cannot access" in seg:
            gone = True
            break
        time.sleep(1)
    if not gone:
        REPORT.record("B1b /dev/vdb 在客机侧消失（pciehp 协作摘除完成）", False,
                      "30s 内 /dev/vdb 仍在——PCIe unplug 未完成，后续断言无意义")
        return

    found, seg = guest_cmd("aether-ota --switch; echo SWITCH_RC=$?",
                           "SWITCH_BAD_DONE", args.cmd_timeout)
    refused = "refusing to switch to unvalidated slot" in seg
    rc1 = GuestConsole.last_group(r"SWITCH_RC=(\d+)", seg)
    REPORT.record("B2 aether-ota 拒绝切换到未校验槽（rc!=0 + refusing）",
                  found and refused and rc1 is not None and rc1 != "0",
                  f"rc={rc1}; {'已拒绝' if refused else '未见拒绝输出'}; 尾部: {tail(seg)}")

    found, seg = guest_cmd("aether-ota --current", "OTA_CUR2_DONE", args.cmd_timeout)
    cur2 = GuestConsole.last_group(r"active_slot=([AB])", seg)
    REPORT.record("B3 拒绝后槽位标记仍为 A（未提交翻转，系统可继续运行）",
                  bool(cur2) and cur2 == "A",
                  f"active_slot={cur2}" if cur2 else "未解析到 active_slot")

    ok, msg = qmp.drive_add("diskB", args.slot_b)
    if not ok:
        REPORT.record("B4a HMP drive_add 恢复 B 盘 backend", False, msg)
        return
    ok, msg = qmp.device_add("diskB", "diskB")
    REPORT.record("B4 QMP device_add 恢复 B 盘", ok, msg if not ok else "diskB 已恢复")
    if not ok:
        return
    # devtmpfs 热插拔：等 /dev/vdb 在客机重现（有界等待）
    ok_dev = False
    for _ in range(20):
        found, seg = guest_cmd("ls /dev/vdb", "VDB_DONE", args.cmd_timeout)
        if found and "/dev/vdb" in seg and "No such file" not in seg:
            ok_dev = True
            break
        time.sleep(1)
    REPORT.record("B5 客机 /dev/vdb 重现（devtmpfs 热插拔真实生效）", ok_dev,
                  "" if ok_dev else "20s 内 /dev/vdb 未重现")

    # ---- C. 切槽并实启 B --------------------------------------------------
    print("[ab-smoke] ==== 阶段 C：切换到 B 槽并重启 ====", flush=True)
    # mark_c 必须在 guest_cmd 之前：--switch 的 request_reboot 命令内同步
    # 触发（同 D1），shutdown 行在 guest_cmd 返回时已出现。
    mark_c = con.mark()
    found, seg = guest_cmd("aether-ota --switch", "SWITCH_DONE", args.cmd_timeout)
    m_sw = re.search(r"switched_slot=([AB]) \(was ([AB])\)", seg)
    switched = m_sw.group(1) if m_sw else None
    was = m_sw.group(2) if m_sw else None
    REPORT.record("C1 aether-ota --switch -> switched_slot=B",
                  bool(switched) and switched == "B" and was == "A",
                  f"switched={switched} (was {was})" if switched else "未解析到 switched_slot")
    if switched != "B":
        return

    found, data = con.wait("=== shutdown: reboot ===", 60, since=mark_c)
    REPORT.record("C2 init 进入优雅重启序列（SIGUSR1 语义）", found,
                  "" if found else f"超时; 日志尾部: {tail(data, mark_c)}")
    if not found:
        return
    found, data = con.wait("reboot: calling reboot(RB_AUTOBOOT)", 60)
    REPORT.record("C3 init 调用 reboot(RB_AUTOBOOT)（PSCI 真实重启）", found,
                  "" if found else f"超时; 日志尾部: {tail(data)}")
    if not found:
        return

    ok, _ = wait_slot_banner("B", args.boot_timeout)
    if not ok:
        return
    ok, _ = login_once("B", args.boot_timeout, args.cmd_timeout)
    if not ok:
        return

    found, seg = guest_cmd("cat /etc/aether-slot", "SLOTMARK2_DONE", args.cmd_timeout)
    slotmark2 = GuestConsole.last_group(r"^([AB])\s*$", seg)
    REPORT.record("C4 客机自报 /etc/aether-slot=B（B 槽镜像身份实证）",
                  bool(slotmark2) and slotmark2 == "B",
                  f"marker={slotmark2!r}" if slotmark2 else "未解析到槽位标记")

    found, seg = guest_cmd("aether-ota --current", "OTA_CUR3_DONE", args.cmd_timeout)
    cur3 = GuestConsole.last_group(r"active_slot=([AB])", seg)
    REPORT.record("C5 aether-ota --current=B（持久标记已翻转）",
                  bool(cur3) and cur3 == "B",
                  f"active_slot={cur3}" if cur3 else "未解析到 active_slot")

    found, seg = guest_cmd("grep ' / ' /proc/mounts", "ROOTMNT2_DONE", args.cmd_timeout)
    rootdev2 = GuestConsole.last_group(r"^(/dev/vd[ab])\s+/\s", seg)
    REPORT.record("C6 根文件系统已变为 /dev/vdb（重启后实启另一槽）",
                  rootdev2 == "/dev/vdb",
                  f"root={rootdev2}" if rootdev2 else f"未解析; 尾部: {tail(seg)}")

    found, seg = guest_cmd("cat /proc/1/comm", "PID1_DONE", args.cmd_timeout)
    REPORT.record("C7 B 槽 PID1 comm=aether-init（原生 init 接管）",
                  found and "aether-init" in seg,
                  next((l.strip() for l in seg.splitlines() if "aether-init" in l), "未见"))

    # ---- D. 恢复模式 ------------------------------------------------------
    print("[ab-smoke] ==== 阶段 D：恢复模式 ====", flush=True)
    # mark_d 必须在 guest_cmd 之前：--mark-recovery 的 request_reboot 是
    # 命令内同步触发的（kill(1,SIGUSR1)），shutdown 行在 guest_cmd 返回时
    # 已经出现——mark 放在命令后会什么也等不到。
    mark_d = con.mark()
    found, seg = guest_cmd("aether-ota --mark-recovery", "MARKREC_DONE", args.cmd_timeout)
    marked = "recovery_marked=1" in seg
    REPORT.record("D1 aether-ota --mark-recovery -> recovery_marked=1",
                  found and marked,
                  "已标记" if marked else f"未见标记输出; 尾部: {tail(seg)}")
    if not marked:
        return

    # since=mark_d：C 段已有一次 shutdown reboot，从头搜会误配 C2 旧行
    found, data = con.wait("=== shutdown: reboot ===", 60, since=mark_d)
    if not found:
        REPORT.record("D2 init 优雅重启（进入恢复前）", False,
                      f"超时; 日志尾部: {tail(data, mark_d)}")
        return
    REPORT.record("D2 init 优雅重启（进入恢复前）", True)

    mark_rec = con.mark()
    found, data = con.wait("AETHEROS_RECOVERY_MODE", args.boot_timeout, since=mark_rec)
    rec_slot = GuestConsole.last_group(r"AETHEROS_RECOVERY_MODE: active_slot=([AB])", data, since=mark_rec)
    REPORT.record("D3 initramfs 进入恢复模式（banner + active_slot=B）",
                  found and rec_slot == "B",
                  f"active_slot={rec_slot}" if rec_slot else
                  ("" if found else f"{args.boot_timeout}s 超时; 日志尾部: {tail(data, mark_rec)}"))
    if not found:
        return

    # since 用 mark_rec 而非新取 mark：恢复序列 banner→apply 仅 ~0.2s（guest
    # 时间），1s 轮询粒度下等 banner 的 wait 返回时 apply 行可能已入日志，
    # 此刻再取 mark 会晚于 apply 行 → 有界 wait 永远等不到（clean7 实锤 FAIL）。
    # mark_rec 取于 banner 等待之前，必早于 banner 与 apply 两行。
    found, data = con.wait("recovery_cleared=1", 120, since=mark_rec)
    m_ap = re.search(r"apply: active_slot=([AB]) validated=(\d)", data[mark_rec:])
    REPORT.record("D4 恢复动作执行：活跃槽校验通过 + 恢复标志已清除",
                  found and m_ap is not None and m_ap.group(2) == "1",
                  f"apply: active_slot={m_ap.group(1)} validated={m_ap.group(2)}"
                  if m_ap else f"未见 apply 输出; 尾部: {tail(data, mark_rec)}")
    if not found:
        return

    ok, _ = wait_slot_banner("B", args.boot_timeout)
    if not ok:
        return
    ok, _ = login_once("D", args.boot_timeout, args.cmd_timeout)
    if not ok:
        return

    found, seg = guest_cmd("cat /etc/aether-slot; aether-ota --current",
                           "FINAL_DONE", args.cmd_timeout)
    final_slot = GuestConsole.last_group(r"^([AB])\s*$", seg)
    final_cur = GuestConsole.last_group(r"active_slot=([AB])", seg)
    REPORT.record("D5 恢复完成后仍启动 slot B（标志清除、无槽位回退）",
                  final_slot == "B" and final_cur == "B",
                  f"marker={final_slot} current={final_cur}")

    # ---- E. 收尾：优雅停机 -------------------------------------------------
    print("[ab-smoke] ==== 阶段 E：优雅停机 ====", flush=True)
    mark = con.mark()
    con.send("aether-power poweroff")
    found, data = con.wait("poweroff: calling reboot(RB_POWER_OFF)", 90, since=mark)
    REPORT.record("E1 init 优雅停机（PSCI poweroff，QEMU 自行退出）", found,
                  "" if found else f"超时; 尾部: {tail(data, mark)}")
    if found:
        try:
            rc = proc.wait(timeout=90)
            REPORT.record("E2 QEMU 自行退出（无外部信号）", rc == 0, f"rc={rc}")
        except subprocess.TimeoutExpired:
            REPORT.record("E2 QEMU 自行退出（无外部信号）", False, "90s 未退出")


if __name__ == "__main__":
    sys.exit(main())
