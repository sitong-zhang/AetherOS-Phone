#!/usr/bin/env python3
"""boot-smoke.py —— AetherOS 真 Linux 内核 QEMU 启动冒烟（CI 断言链）

对「真 ARM64 Linux 内核 + 真 ext4 rootfs + AetherOS 原生 init」做端到端断言。
所有期望字面值均取自沙箱真实取证日志（aether-init-boot-evidence.log /
aether-init-flap-evidence.log），不是想象中的输出。

断言链（任一失败即整体失败，退出码 1）：
  A. 启动取证   : AETHEROS_BOOT_DIAG_DONE + [DIAG] pid1 comm=aether-init
                  + service startup complete (N services, 0 failed)
  B. 登录        : getty 登录提示 -> root -> shell 回显 LOGIN_MARKER_OK
  C. PID 1 身份  : E1_PID1_COMM=aether-init
  D. 服务拓扑    : PID1=aether-init; aetherosd/getty 均 PPID=1
  E. IPC 准入    : /run/aetheros.sock 存在且 mode 0660 (srw-rw----)
  F. IPC 契约    : aetherctl -c -> schema/backend/virtio_disk/fstype/hostname/cpus
                  + 两次调用 uptime_s 递增（活数据）
  G. 9p 共享     : "9p: hostshare mounted" + JSON "share_mounted": true（给 --share 时）
  H. 崩溃自恢复  : kill -9 aetherosd -> init "restarting" -> 新 PID（且 != 旧 PID）
                  -> 重启后 IPC 契约仍成立
  I. 优雅停机    : aether-power poweroff -> === shutdown: poweroff ===
                  -> poweroff: calling reboot(RB_POWER_OFF) -> QEMU 自行退出
  J. 图形栈(P0-6): virtio-gpu DRM 节点 + 合成器进程(PPID=1) + IPC 契约
                  (aethercomp.state/1) + 空闲帧哈希稳定 + focus 后哈希变化
                  + screenshot 落盘 PPM 头合法 + close 后窗口数递减
  K. 真实输入(P1-2a): evdev 键盘识别 + QMP send-key Tab 焦点切换
  S. 安全沙箱(P1-2b): 应用 uid=1000 + CapEff=0 + Seccomp=2 + NoNewPrivs=1
  P. 包管理器(P2-11): demo.aepkg 开机安装注册 + 运行时安装 hello.aepkg
                  + 篡改拒绝 + 外国密钥拒绝 + 安装应用沙箱运行
  U. SystemUI(P3-14): systemui PPID=1 + 窗口在 compositor list + 状态读数
                  与 /proc 实测交叉验证 + 应用网格来自真实包索引
                  + 数字键/IPC 两条启动链路 + 截图像素
  L. 序列化日志(P2-13a): logd socket/PPID=1 + 客户端->落盘全链 + 行格式
                  + services.state + SIGUSR1 轮转
  W. 软看门狗(P2-13c): watchdog PPID=1 + 形态如实报告 + SIGSTOP 冻结
                  aetherosd -> 心跳超时处决 -> init 拉新 pid
  C. 崩溃管理器(P2-13b): crashd socket/PPID=1 + SIGKILL 报告(signal 9)
                  + kill -SEGV halandroid 报告(signal 11) + 诚实边界 + halandroid 重启

纪律：不猜输出、不放宽断言、失败时打印日志尾部便于定位。
"""
import argparse
import glob
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
        self.checks = []  # (name, ok, detail)；ok=None 表示 SKIP（宿主不具备运行条件）

    def record(self, name, ok, detail=""):
        self.checks.append((name, ok, detail))
        mark = "PASS" if ok else "FAIL"
        line = f"[{mark}] {name}"
        if detail:
            line += f" —— {detail}"
        print(line, flush=True)

    def skip(self, name, detail=""):
        """如实 SKIP：宿主不具备运行条件（如无 /dev/dri render node），
        不计 FAIL、更不伪造 PASS——铁律：不假实现冒充真实现。"""
        self.checks.append((name, None, detail))
        line = f"[SKIP] {name}"
        if detail:
            line += f" —— {detail}"
        print(line, flush=True)

    @property
    def failed(self):
        return [c for c in self.checks if c[1] is False]

    @property
    def skipped(self):
        return [c for c in self.checks if c[1] is None]

    def summary_text(self):
        lines = ["AetherOS boot smoke summary", "=" * 40]
        for name, ok, detail in self.checks:
            mark = "PASS" if ok else ("SKIP" if ok is None else "FAIL")
            lines.append(f"[{mark}] {name}" + (f" —— {detail}" if detail else ""))
        n_fail = len(self.failed)
        n_skip = len(self.skipped)
        lines.append("=" * 40)
        verdict = "OK" if n_fail == 0 else "FAILED"
        if n_fail == 0 and n_skip:
            verdict = "OK（含 SKIP）"
        lines.append(
            f"total={len(self.checks)} failed={n_fail} skipped={n_skip} "
            f"result={verdict}")
        return "\n".join(lines) + "\n"


REPORT = Report()


# ---------------------------------------------------------------- 控制台

class GuestConsole:
    """经 FIFO 驱动 QEMU 串口；日志落盘；所有等待都带截止时间。"""

    def __init__(self, log_path):
        self.log_path = log_path
        self.fifo_path = log_path + ".console.in"
        for p in (self.fifo_path,):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        os.mkfifo(self.fifo_path)
        self.fin = os.open(self.fifo_path, os.O_RDWR)  # O_RDWR: 不阻塞且保持写端
        self._logf = open(log_path, "wb")

    def read_log(self):
        try:
            with open(self.log_path, "rb") as f:
                return f.read().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def send(self, line):
        os.write(self.fin, (line + "\n").encode())

    def wait(self, marker, timeout, since=0, note=""):
        """等待 marker 出现在日志[since:]中。返回 (found, tail)。

        仅用于**非本端 echo 产生**的标记（内核/init 日志行、getty 提示、
        带前缀的行如 "[DIAG] ..."）。对冒烟自己 `echo MARKER` 产生的
        完成标记必须用 wait_line()——见其 docstring 的回显误判说明。
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = self.read_log()
            if marker in data[since:]:
                return True, data
            time.sleep(1.0)
        return False, self.read_log()

    def wait_line(self, marker, timeout, since=0, note=""):
        """等待日志[since:]中出现**整行等于** marker 的输出行。

        为什么不能只用 wait()：冒烟发给客机的命令本身含
        `...; echo MARKER`，getty 行编辑器会把整条命令回显出来，
        wait() 的 substring 匹配会命中**命令回显**、在命令真正执行完
        之前就返回——P5/H1 假 FAIL 的实锤根因（2026-10 取证：
        p5forensic 显示同一条命令实际 ~12s 正常完成，
        uid=1000/CapEff=0/Seccomp=2 全部满足，只是断言抢跑）。
        行级精确匹配天然免疫：回显行要么是整条命令、要么是 80 列
        wrap 片段（busybox ash 行编辑器按终端宽折行），
        没有一行会恰好只剩 MARKER 本身。
        pty 输出的 \\r 由 strip() 处理；轮询 0.5s 兼顾延迟与 CPU。
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            data = self.read_log()
            if any(l.strip() == marker for l in data[since:].splitlines()):
                # 二次确认：80 列 wrap 可能恰好把「; echo MARKER」折成
                # 只剩 MARKER 的回显行（如 U1 的 ps|awk 命令），行级匹配
                # 会命中它、在命令真正产出前返回。短暂再读一次，让真实
                # 输出行（在 echo 之前的命令产出）有机会进入 data。
                time.sleep(0.15)
                data = self.read_log()
                return True, data
            time.sleep(0.5)
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

    用于 P1-2a：向客机注入真实键盘事件 send-key，验证合成器 evdev 路径。
    握手：收到 {'QMP':...} 后发 {'execute':'qmp_capabilities'}，再发命令。
    """

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

    def send_key(self, qcode):
        self.f.write((json.dumps({
            "execute": "send-key",
            "arguments": {"keys": [{"type": "qcode", "data": qcode}]},
        }) + "\n").encode())
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
        """注入一次绝对指针点击：移动到 (x,y) 后按下/抬起 button。
        QMP input-send-event 用的是通用事件类型 abs/btn（没有 "tablet"
        这种类型）；绝对坐标走 abs 事件、axis 取 "x"/"y"，其值落在
        virtio-tablet 的逻辑轴范围 [0,0x7FFF] 内（与客机 EVIOCGABS 读到的
        absmax 一致），合成器据此映射到屏幕像素。"""
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


# ---------------------------------------------------------------- QEMU

def start_qemu(args):
    cmd = [
        args.qemu, "-M", "virt", "-cpu", "cortex-a57", "-smp", "2", "-m", args.memory,
        "-kernel", args.kernel,
        "-drive", f"if=virtio,format=raw,file={args.rootfs}",
        "-append", "console=ttyAMA0 root=/dev/vda rw ip=dhcp",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0",
    ]
    if not args.no_gpu:
        # 分辨率随形态走：默认 1280x800（横屏基线），手机竖屏用 720x1600
        # （1080x2400 手机的原生 9:20 比例，VNC 里铺满整块手机屏）。
        res_opt = f",xres={args.xres},yres={args.yres}"
        if args.gpu_virgl:
            # P1-2e: 开启 virgl（VIRTIO_GPU_F_VIRGL），需宿主 GL 显示后端。
            # 设备选型踩坑：本 QEMU（Debian 8.2 模块化构建）没有
            # virtio-gpu-pci 的 virgl 属性，virgl 的载体是独立设备
            # virtio-gpu-gl-pci（ui-opengl/hw-display-virtio-gpu-gl 模块）。
            # 合成器仍走软件 dumb-buffer + KMS，与 virgl 解耦。
            cmd += ["-device", "virtio-gpu-gl-pci" + res_opt]
        else:
            # P0-6 / 默认：软件光栅化路径（无 3D 命令流）。
            cmd += ["-device", "virtio-gpu-pci" + res_opt]
    if args.keyboard:
        # P1-2a: 真实 virtio 键盘设备；内核 CONFIG_VIRTIO_INPUT=y 驱动，
        # 合成器经 /dev/input/event* 与 EVIOCGBIT 探测到真实键盘。
        cmd += ["-device", "virtio-keyboard-pci"]
    if args.tablet:
        # 触摸/指针：virtio-tablet-pci 暴露绝对坐标 (ABS_X/ABS_Y) + BTN_TOUCH，
        # 合成器经 /dev/input/event* 探测为 tablet 并把命中 SystemUI 的点击
        # 经 SYSUI_SOCK `tap` 命令转发，SystemUI 做 Launcher 行命中启动应用。
        cmd += ["-device", "virtio-tablet-pci"]
    if args.share:
        cmd += ["-virtfs", f"local,path={args.share},mount_tag=hostshare,"
                           "security_model=passthrough,id=hostshare"]
    # QMP 控制通道（用于 P1-2a 注入真实按键 send-key）；始终开启，开销可忽略。
    cmd += [f"-chardev", f"socket,path={args.log}.qmp,server=on,wait=off,id=qmp",
            "-mon", "chardev=qmp,mode=control"]
    # -snapshot：客机所有写进入临时覆盖层，QEMU 退出即弃。
    # 动机（P2-11 实锤）：冒烟会在客机里真实安装 .aepkg（P2 段）——
    # 没有 -snapshot 时每次跑都把 rootfs 镜像写脏；若 QEMU 被 SIGTERM
    # 非干净杀死（复现/调试路径常见），ext4 journal 恢复后元数据在、
    # 数据块丢，下次启动装过的应用二进制变成全零文件（execvp 遇
    # ENOEXEC 回落 /bin/sh → 静默 exit 0），P5 断言假 FAIL。
    # 单 QEMU 进程内的客机重启（I 段 poweroff）不受影响。
    cmd += ["-snapshot"]
    cmd += ["-display", "none" if not args.gpu_virgl else "egl-headless",
            "-serial", "stdio", "-monitor", "none"]
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


# ---------------------------------------------------------------- 主流程

def main():
    global con
    ap = argparse.ArgumentParser(description="AetherOS QEMU boot smoke (strict assertion chain)")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--rootfs", required=True)
    ap.add_argument("--share", default="")
    ap.add_argument("--log", default="/tmp/aether-boot-smoke.log")
    ap.add_argument("--summary", default="/tmp/aether-boot-smoke.summary.txt")
    ap.add_argument("--qemu", default="qemu-system-aarch64")
    ap.add_argument("--memory", default="512M")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--cmd-timeout", type=int, default=60)
    ap.add_argument("--no-gpu", action="store_true",
                    help="不附加 virtio-gpu-pci（跳过 J 段前置条件）")
    ap.add_argument("--keyboard", action="store_true",
                    help="附加 virtio-keyboard-device（P1-2a 真实键盘，需内核 CONFIG_VIRTIO_INPUT）")
    ap.add_argument("--tablet", action="store_true",
                    help="附加 virtio-tablet-pci（触摸/指针，合成器路由点击到 SystemUI Launcher）")
    ap.add_argument("--gpu-virgl", action="store_true",
                    help="virtio-gpu-gl-pci 开启 virgl 3D 能力（P1-2e；需宿主 render node）")
    ap.add_argument("--xres", type=int, default=1280,
                    help="virtio-gpu 水平分辨率（默认 1280；手机竖屏形态用 720）")
    ap.add_argument("--yres", type=int, default=800,
                    help="virtio-gpu 垂直分辨率（默认 800；手机竖屏形态用 1600）")
    args = ap.parse_args()

    for p in (args.kernel, args.rootfs):
        if not os.path.isfile(p):
            print(f"[FATAL] 输入不存在: {p}", file=sys.stderr)
            return 2

    # 宿主前置探测（P1-2e）：QEMU 的 virgl（virtio-gpu-gl-pci）经 ui-opengl
    # 模块初始化宿主 EGL，硬依赖 DRM render node；llvmpipe 软件 GL 同样要
    # render node（kms_swrast 走 render node ioctl）。宿主无 /dev/dri 且
    # mknod 被禁（含 user namespace）时，如实 SKIP G 段而非伪造 PASS 或
    # 虚假 FAIL——其余段由默认（非 virgl）冒烟完整覆盖。
    if args.gpu_virgl:
        rn = sorted(glob.glob("/dev/dri/renderD*"))
        if not rn:
            REPORT.skip("G1 virgl 3D 能力上报 cap_3d=1（VIRTGPU_PARAM_3D_FEATURES）",
                        "宿主无 /dev/dri/renderD*（QEMU ui-opengl/virgl 必需）")
            REPORT.skip("G2 合成器启动日志报 VIRTGPU_PARAM_3D_FEATURES=1",
                        "同上：宿主不具备 virgl 运行条件，G 段未执行")
            with open(args.summary, "w") as f:
                f.write(REPORT.summary_text())
            print("[smoke] host lacks DRM render node -> G-section SKIP (honest skip)")
            return 0

    # Android 宿主 HAL 端到端：启动前向 9p share 写一份合成快照。客户机
    # aether-hal-android 应读到 REAL，SystemUI 状态栏显示该电量（U3d 断言）。
    if args.share:
        os.makedirs(args.share, exist_ok=True)
        with open(os.path.join(args.share, "android-host.json"), "w") as _hf:
            _hf.write('{"battery":77,"charging":1,"wifi":"AetherLab",'
                      '"cellular":"","notifications":3}\n')

    con = GuestConsole(args.log)
    proc = start_qemu(args)
    exit_code = 1
    try:
        try:
            run_checks(args, proc)
        except Exception as e:  # 未预期错误也必须落成失败检查项，而非裸 traceback
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
        print(f"[smoke] summary -> {args.summary}")
        exit_code = 0 if not REPORT.failed else 1
    return exit_code


def run_checks(args, proc):
    # ---- A. 启动取证 ----------------------------------------------------
    # 注意：boot diag 的完成行是 "[DIAG] AETHEROS_BOOT_DIAG_DONE"（带前缀，
    # make_rootfs.sh 的 echo 决定），必须用 substring 的 wait()——
    # 2026-10-19 实锤：批量 sed 把 _DONE 后缀全切 wait_line 时误伤此行，
    # 行级精确匹配永远等不到带前缀的行，A1 假超时 240s、后续全部没跑。
    found, data = con.wait("AETHEROS_BOOT_DIAG_DONE", args.boot_timeout,
                           note="boot diag")
    if not found and proc.poll() is not None:
        REPORT.record("A1 启动取证: boot diag 完成", False,
                      f"QEMU 提前退出 rc={proc.returncode}; 日志尾部: {tail(data)}")
        return
    REPORT.record("A1 启动取证: AETHEROS_BOOT_DIAG_DONE", found,
                  "" if found else f"{args.boot_timeout}s 超时; 日志尾部: {tail(data)}")
    if not found:
        return

    REPORT.record("A2 启动取证: guest 自报 PID1=aether-init",
                  "pid=1 comm=aether-init" in data)
    m = re.search(r"service startup complete \((\d+) services, (\d+) failed\)", data)
    REPORT.record("A3 启动取证: 服务拓扑 0 失败",
                  bool(m) and m.group(2) == "0",
                  m.group(0) if m else "未找到 startup complete 行")

    # ---- B. 登录 --------------------------------------------------------
    found, data = con.wait("login:", args.cmd_timeout)
    REPORT.record("B1 getty 登录提示出现", found,
                  "" if found else f"超时; 日志尾部: {tail(data)}")
    if not found:
        return
    con.send("root")
    time.sleep(1.0)
    con.send("")  # root 空密码（/etc/shadow: root::）
    time.sleep(1.0)
    mark = con.mark()
    con.send("echo LOGIN_MARKER_OK")
    found, data = con.wait_line("LOGIN_MARKER_OK", args.cmd_timeout, since=mark)
    REPORT.record("B2 shell 回显 LOGIN_MARKER_OK", found,
                  "" if found else f"超时; 日志尾部: {tail(data, mark)}")
    if not found:
        return

    # ---- C. PID 1 身份 --------------------------------------------------
    mark = con.mark()
    con.send("echo E1_PID1_COMM=$(cat /proc/1/comm)")
    found, data = con.wait("E1_PID1_COMM=aether-init", args.cmd_timeout, since=mark)
    REPORT.record("C1 PID 1 comm=aether-init", found,
                  "" if found else f"超时; 日志尾部: {tail(data, mark)}")
    if not found:
        return

    # ---- D. 服务拓扑 ----------------------------------------------------
    mark = con.mark()
    con.send("ps -o pid,ppid,stat,comm; echo TOPOLOGY_DONE")
    found, data = con.wait_line("TOPOLOGY_DONE", args.cmd_timeout, since=mark)
    if not found:
        REPORT.record("D1 服务拓扑: ps 输出", False, f"超时; 日志尾部: {tail(data, mark)}")
        return
    seg = data[mark:]
    pid1 = re.search(r"^\s*1\s+\d+\s+\S+\s+aether-init\s*$", seg, re.MULTILINE)
    REPORT.record("D1 PID 1 进程是 aether-init", bool(pid1),
                  pid1.group(0).strip() if pid1 else "ps 中未见 PID1=aether-init")
    svc = re.search(r"^\s*\d+\s+1\s+\S+\s+aetherosd\s*$", seg, re.MULTILINE)
    REPORT.record("D2 aetherosd 运行且 PPID=1", bool(svc),
                  svc.group(0).strip() if svc else "ps 中未见 PPID=1 的 aetherosd")
    # getty 经 exec 链变换 comm（busybox getty -> login -> sh，同一 pid），
    # 因此不断言 comm 字面值，而是锚定 init 拉起的服务 pid：它必须存活且 PPID=1。
    getty_svc = None
    for m in re.finditer(r"service 'getty' running \(pid=(\d+)\)", data):
        getty_svc = m  # 取最后一次（若曾重启）
    if getty_svc:
        gpid = getty_svc.group(1)
        alive = re.search(rf"^\s*{gpid}\s+1\s+\S+\s+\S+\s*$", seg, re.MULTILINE)
        REPORT.record("D3 getty 服务进程存活且 PPID=1", bool(alive),
                      f"服务 pid={gpid} -> {alive.group(0).strip() if alive else 'ps 中未见该 pid'}"
                      f"（comm 在 getty->login->sh exec 链中变换，属预期）")
    else:
        REPORT.record("D3 getty 服务进程存活且 PPID=1", False,
                      "init 日志中未见 service 'getty' running (pid=)")

    # ---- E. IPC 准入（0660）---------------------------------------------
    mark = con.mark()
    con.send("ls -la /run/aetheros.sock; echo SOCK_DONE")
    found, data = con.wait_line("SOCK_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    REPORT.record("E1 /run/aetheros.sock mode 0660 (srw-rw----)",
                  found and "srw-rw----" in seg,
                  extract_sock_line(seg) if found else "SOCK_DONE 超时")

    # ---- F. IPC 契约 ----------------------------------------------------
    mark = con.mark()
    con.send("aetherctl -c; echo IPC1_DONE")
    found, data = con.wait_line("IPC1_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    if not found:
        REPORT.record("F1 IPC 契约: aetherctl -c", False, "IPC1_DONE 超时")
    else:
        for desc, lit in [
            ("F1 schema=aetherosd.state/1", '"schema": "aetherosd.state/1"'),
            ("F2 backend=linux-native", '"backend": "linux-native"'),
            ("F3 virtio_disk=true（真 virtio 磁盘）", '"virtio_disk": true'),
            ("F4 root fstype=ext4（真磁盘 rootfs）", '"fstype": "ext4"'),
            ("F5 hostname=aetheros（本 rootfs 身份）", '"hostname": "aetheros"'),
        ]:
            REPORT.record(desc, lit in seg)
        cpus = re.search(r'"cpus": (\d+)', seg)
        REPORT.record("F6 cpus>=1（真实读数）",
                      bool(cpus) and int(cpus.group(1)) >= 1,
                      f"cpus={cpus.group(1)}" if cpus else "未解析到 cpus")
        u1 = re.search(r'"uptime_s": (\d+)', seg)
        if u1:
            time.sleep(3)
            mark2 = con.mark()
            con.send("aetherctl -c; echo IPC2_DONE")
            found2, data2 = con.wait_line("IPC2_DONE", args.cmd_timeout, since=mark2)
            seg2 = data2[mark2:]
            u2 = re.search(r'"uptime_s": (\d+)', seg2)
            if u2:
                REPORT.record("F7 活数据: uptime_s 递增",
                              int(u2.group(1)) > int(u1.group(1)),
                              f"{u1.group(1)} -> {u2.group(1)}")
            else:
                REPORT.record("F7 活数据: uptime_s 递增", False, "第二次调用未解析到 uptime_s")
        else:
            REPORT.record("F7 活数据: uptime_s 递增", False, "首次调用未解析到 uptime_s")

    # ---- G. 9p 共享（提供 --share 时）-----------------------------------
    if args.share:
        REPORT.record("G1 9p hostshare 挂载", "9p: hostshare mounted at /mnt/share" in data)
        REPORT.record("G2 JSON share_mounted=true", '"share_mounted": true' in data)

    # ---- J. 图形栈（P0-6：DRM/KMS + 合成器）-----------------------------
    if args.no_gpu:
        REPORT.record("J0 virtio-gpu 已附加", True, "跳过（--no-gpu）")
    else:
        run_graphics_checks(args, data)

    # ---- K. evdev 真实键盘（P1-2a）--------------------------------------
    if args.keyboard:
        run_keyboard_checks(args, data)

    # ---- T. 触摸/指针（合成器路由点击 -> SystemUI Launcher 启动）----------
    if args.tablet:
        run_touch_checks(args, data)

    # ---- G. virgl GPU 3D 能力（P1-2e）-----------------------------------
    if args.gpu_virgl and not args.no_gpu:
        run_virgl_checks(args, data)

    # ---- S. 安全沙箱（P1-2b：demo 以非 root + 降权 + seccomp 运行）--------
    run_sandbox_checks(args)

    # ---- P. 包管理器（P2-11：.aepkg 验签/安装/索引/沙箱运行）--------------
    run_pkg_checks(args)

    # ---- U. SystemUI（P3-14：状态条 + Launcher + 真实启动链路）------------
    run_sysui_checks(args)

    # ---- L. 序列化日志（P2-13a：aether-logd）------------------------------
    run_log_checks(args)

    # ---- H. 崩溃自恢复 ---------------------------------------------------
    mark = con.mark()
    con.send("AP=$(ps -o pid=,comm= | awk '$2==\"aetherosd\"{print $1}'); echo E2_BEFORE_PID=$AP; echo BEFORE_DONE")
    found, data = con.wait_line("BEFORE_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    before = GuestConsole.last_group(r"E2_BEFORE_PID=(\d+)", seg)
    REPORT.record("H1 取得 aetherosd 旧 PID", bool(before), f"pid={before}" if before else "未解析")
    if not before:
        return
    mark = con.mark()
    con.send(f"kill -9 {before}; echo E2_KILLED={before}; echo KILL_DONE")
    found, data = con.wait_line("KILL_DONE", args.cmd_timeout, since=mark)
    REPORT.record("H2 kill -9 已执行", found, "" if found else "KILL_DONE 超时")
    if not found:
        return
    # 只认本次 kill 之后产生的监管行（since=mark），防止启动期旧重启记录误配
    found, data = con.wait("service 'aetherosd' exited (code=137) -> restarting", 30, since=mark)
    REPORT.record("H3 init 监管发现崩溃并重启", found,
                  "" if found else f"30s 内未见 restarting 行; 日志尾部: {tail(data, mark)}")
    if not found:
        return
    found, data = con.wait("restarted (pid=", 30, since=mark)
    newpid = GuestConsole.last_group(r"service 'aetherosd' restarted \(pid=(\d+)\)", data, since=mark)
    REPORT.record("H4 init 拉起新 aetherosd 进程", bool(newpid),
                  f"新 pid={newpid}" if newpid else "未见 restarted (pid=) 行")
    if not newpid:
        return
    REPORT.record("H5 新 PID != 旧 PID（真重启，非同一进程）",
                  newpid != before, f"{before} -> {newpid}")
    mark = con.mark()
    con.send("echo E2_AFTER_PID=$(ps -o pid=,comm= | awk '$2==\"aetherosd\"{print $1}'); echo AFTER_DONE")
    found, data = con.wait_line("AFTER_DONE", args.cmd_timeout, since=mark)
    after = GuestConsole.last_group(r"E2_AFTER_PID=(\d+)", data[mark:])
    REPORT.record("H6 新 PID 与 ps 实测一致", after == newpid,
                  f"init 报 {newpid} / ps 实测 {after}")

    # H7: 重启后 IPC 契约仍成立
    mark = con.mark()
    con.send("aetherctl -c; echo IPC3_DONE")
    found, data = con.wait_line("IPC3_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    REPORT.record("H7 重启后 IPC 契约仍成立",
                  found and '"schema": "aetherosd.state/1"' in seg,
                  "" if (found and '"schema": "aetherosd.state/1"' in seg) else "schema 缺失或超时")

    # ---- W. 软看门狗（P2-13c：aether-watchdog 发现 IPC 僵死并杀）----------
    run_watchdog_checks(args)

    # ---- C. 崩溃管理器（P2-13b：aether-crashd 结构化报告）------------------
    run_crash_checks(args)

    # ---- I. 优雅停机 -----------------------------------------------------
    mark = con.mark()
    con.send("aether-power poweroff")
    found, data = con.wait("=== shutdown: poweroff ===", 30, since=mark)
    REPORT.record("I1 init 进入优雅停机序列", found,
                  "" if found else f"超时; 日志尾部: {tail(data, mark)}")
    if not found:
        return
    found, data = con.wait("poweroff: calling reboot(RB_POWER_OFF)", 30)
    REPORT.record("I2 init 调用 reboot(RB_POWER_OFF)（PSCI）", found,
                  "" if found else f"超时; 日志尾部: {tail(data)}")
    if not found:
        return
    try:
        rc = proc.wait(timeout=90)
        REPORT.record("I3 QEMU 自行退出（PSCI poweroff，无外部信号）", rc == 0, f"rc={rc}")
    except subprocess.TimeoutExpired:
        REPORT.record("I3 QEMU 自行退出（PSCI poweroff，无外部信号）", False,
                      "90s 未退出——优雅停机失败")


def tail(data, since=0, n=1200):
    seg = data[since:] if since else data
    return seg[-n:].replace("\n", " | ")[-n:]


def extract_sock_line(seg):
    for line in seg.splitlines():
        if "aetheros.sock" in line:
            return line.strip()
    return "(未见 socket 行)"


def _wait_idle_frame(con, args, stable_s=1.2, timeout=40):
    """轮询 compositor frame 计数，直到连续 stable_s 秒不再前进。

    用于 J9：任何迟到的重绘源（SystemUI 未冻结时的定时刷新、迟到的应用
    窗口动画）都可能晚于测试起点仍在跑。必须在 frame 真正停止前进后再
    断言空闲稳定，否则抢采必然哈希跳动——那是测试时序敏感，不是系统 bug。

    若 frame 永远前进（动画循环 / 无限重绘）则超时返回 False：这恰恰是
    要抓的真实 bug，本函数不会为之放水。"""
    last = None
    stable_since = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        mark = con.mark()
        con.send("aethercompctl -c status; echo IDLE_PROBE_DONE")
        found, data = con.wait_line("IDLE_PROBE_DONE", args.cmd_timeout, since=mark)
        if not found:
            return False, last
        m = re.search(r'"frame":(\d+)', data[mark:])
        if not m:
            time.sleep(0.2)
            continue
        fr = int(m.group(1))
        now = time.time()
        if fr == last:
            if stable_since is None:
                stable_since = now
            elif now - stable_since >= stable_s:
                return True, fr
        else:
            stable_since = None
        last = fr
        time.sleep(0.2)
    return False, last


def run_graphics_checks(args, boot_data):
    """J 段：图形栈全链路断言（P0-6）。boot_data 为截至目前的完整串口日志。"""
    # P3-14：SystemUI 会定时重绘（时钟/内存读数），这是一个持续变化的像素源。
    # J9 的「空闲帧哈希稳定」要求同一输入下渲染确定性——先把这个动源显式
    # 冻结（refresh off），测完再恢复（refresh on）。这是确定化测试输入，
    # 不是放宽断言：冻结期间哈希必须严格一致，恢复后另设断言验证它在刷。
    mark = con.mark()
    con.send("aether-sysuictl -c 'refresh off' >/dev/null 2>&1; "
             "echo SYSUI_FREEZE_DONE")
    con.wait_line("SYSUI_FREEZE_DONE", args.cmd_timeout, since=mark)

    # J1/J2: bootdiag 取证行（DRM 节点 + 合成器 socket 权限）
    drm_line = next((l.strip() for l in boot_data.splitlines()
                     if "[DIAG] drm node" in l), "")
    REPORT.record("J1 bootdiag: /dev/dri/card0 字符设备存在",
                  "card0" in drm_line and "crw-" in drm_line,
                  drm_line or "未见 [DIAG] drm node 行")
    comp_sock_line = next((l.strip() for l in boot_data.splitlines()
                           if "[DIAG] comp socket" in l), "")
    REPORT.record("J2 bootdiag: /run/aetheros-comp.sock mode 0660 (srw-rw----)",
                  "srw-rw----" in comp_sock_line,
                  comp_sock_line or "未见 [DIAG] comp socket 行")

    # J3: 合成器进程存活且由 init 监管（PPID=1）
    # 注意：内核 comm 上限 15 字符（TASK_COMM_LEN=16 含 NUL），
    # "aether-compositor" 在 ps 中显示为截断的 "aether-composit"。
    mark = con.mark()
    con.send("ps -o pid,ppid,stat,comm; echo COMP_PS_DONE")
    found, data = con.wait_line("COMP_PS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    comp = re.search(r"^\s*(\d+)\s+1\s+\S+\s+aether-composit(or)?\s*$", seg,
                     re.MULTILINE)
    REPORT.record("J3 合成器进程运行且 PPID=1", bool(comp),
                  comp.group(0).strip() if comp else "ps 中未见 PPID=1 的 aether-compositor")
    if not comp:
        return

    # J4: IPC 契约
    mark = con.mark()
    con.send("aethercompctl -c status; echo COMP_STATUS_DONE")
    found, data = con.wait_line("COMP_STATUS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    if not found:
        REPORT.record("J4 合成器 IPC 契约", False, "COMP_STATUS_DONE 超时")
        return
    for desc, pat in [
        ("J4 schema=aethercomp.state/1", r'"schema":\s*"aethercomp\.state/1"'),
        ("J5 mode=WxH（KMS 模式）", r'"mode":\s*"\d+x\d+"'),
        ("J6 windows>=1（WM 管理窗口）", r'"windows":\s*([1-9]|\d\d+)'),
        ("J7 hash 为 16 位十六进制", r'"hash":\s*"[0-9a-f]{16}"'),
    ]:
        m = re.search(pat, seg)
        REPORT.record(desc, bool(m), m.group(0) if m else "未匹配")

    # J17: 硬件 plane 清单（P1-2d）。合成器枚举真实 DRM 资源并如实上报
    # total/primary/overlay/cursor；virtio-gpu 只暴露 primary，故
    # overlay/cursor=0 是诚实的硬件事实，而非功能缺失的假声称。
    m = re.search(r'"planes":\s*\{\s*"total":\s*(\d+),\s*"primary":\s*(\d+),'
                  r'\s*"overlay":\s*(\d+),\s*"cursor":\s*(\d+)\}', seg)
    if m:
        tot, prim, ov, cur = (int(m.group(i)) for i in range(1, 5))
        REPORT.record("J17 硬件 plane 清单已上报且 primary>=1（枚举真实资源）",
                      prim >= 1 and tot >= prim,
                      f"total={tot} primary={prim} overlay={ov} cursor={cur}；"
                      "overlay/cursor=0 如实反映 virtio-gpu 仅 primary")
    else:
        REPORT.record("J17 硬件 plane 清单已上报且 primary>=1", False, "status 未见 planes 字段")
    mode_m = re.search(r'"mode":\s*"(\d+)x(\d+)"', seg)
    if not mode_m:
        REPORT.record("J8 mode 解析失败（后续断言中止）", False, "")
        return
    mode_w, mode_h = int(mode_m.group(1)), int(mode_m.group(2))

    # J9: 空闲帧哈希稳定（无状态变化不重绘）
    # 先等系统真正空闲：轮询 frame 直到连续 ~1.2s 不再前进（demo 入场动画
    # 已播完 + SystemUI 已冻结，无脏标记）。这测的是「空闲后渲染确定性」这一
    # 真实性质，而非在动画进行中抢采——之前在 TCG 慢启动下误 FAIL 即源于此。
    idle_ok, idle_frame = _wait_idle_frame(con, args)
    if not idle_ok:
        REPORT.record("J9 空闲帧哈希稳定（3s 间隔两次一致）", False,
                      f"等待空闲超时（最后 frame={idle_frame} 仍在变化，"
                      "疑似动画/重绘未终止——真实 bug）")
    else:
        mark = con.mark()
        con.send("aethercompctl -c hash; echo COMP_HASH1_DONE")
        found, data = con.wait_line("COMP_HASH1_DONE", args.cmd_timeout, since=mark)
        h1 = re.search(r'"hash":\s*"([0-9a-f]{16})"', data[mark:])
        time.sleep(3)
        mark = con.mark()
        con.send("aethercompctl -c hash; echo COMP_HASH2_DONE")
        found, data = con.wait_line("COMP_HASH2_DONE", args.cmd_timeout, since=mark)
        h2 = re.search(r'"hash":\s*"([0-9a-f]{16})"', data[mark:])
        REPORT.record("J9 空闲帧哈希稳定（3s 间隔两次一致）",
                      bool(h1 and h2 and h1.group(1) == h2.group(1)),
                      f"idle frame={idle_frame} | "
                      f"{h1.group(1) if h1 else '?'} vs {h2.group(1) if h2 else '?'}")

    # 恢复 SystemUI 定时重绘（J9 测完）——U 段会验证它真的在刷
    mark = con.mark()
    con.send("aether-sysuictl -c 'refresh on' >/dev/null 2>&1; echo SYSUI_RESUME_DONE")
    con.wait_line("SYSUI_RESUME_DONE", args.cmd_timeout, since=mark)

    # J10: focus 切换后帧哈希变化（真渲染，非假画面）
    # P4-1：开机窗口表只剩 SystemUI 全屏锁屏（不再播种调试窗口，demo 也不再是
    # 开机服务），单窗口无从切换焦点。这里先经 `open` 开两个服务端渲染窗口
    # （K/J 段同款调试窗口），再在两者间切换 focus——焦点环颜色（青/暗）不同
    # 于焦点环像素，哈希必须变化。
    j12_close_id = None
    mark = con.mark()
    con.send("aethercompctl -c open dbg-a; aethercompctl -c open dbg-b; "
             "echo COMP_OPEN_DONE")
    found, data = con.wait_line("COMP_OPEN_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    open_ids = re.findall(r'"focused":\s*(\d+)', seg)
    if len(open_ids) < 2:
        REPORT.record("J10 focus 切换后帧哈希变化（真渲染路径）", False,
                      f"open 两个窗口失败: {tail(seg)}")
    else:
        id_a, id_b = open_ids[0], open_ids[1]
        time.sleep(1)
        mark2 = con.mark()
        con.send("aethercompctl -c hash; echo COMP_HASH_BASE_DONE")
        found, data = con.wait_line("COMP_HASH_BASE_DONE", args.cmd_timeout,
                                    since=mark2)
        hb = re.search(r'"hash":\s*"([0-9a-f]{16})"', data[mark2:])
        mark3 = con.mark()
        con.send(f"aethercompctl -c focus {id_a}; echo COMP_FOCUS_A_DONE")
        con.wait_line("COMP_FOCUS_A_DONE", args.cmd_timeout, since=mark3)
        time.sleep(1)
        mark4 = con.mark()
        con.send("aethercompctl -c hash; echo COMP_HASH_FA_DONE")
        found, data = con.wait_line("COMP_HASH_FA_DONE", args.cmd_timeout,
                                    since=mark4)
        hfa = re.search(r'"hash":\s*"([0-9a-f]{16})"', data[mark4:])
        mark5 = con.mark()
        con.send(f"aethercompctl -c focus {id_b}; echo COMP_FOCUS_B_DONE")
        con.wait_line("COMP_FOCUS_B_DONE", args.cmd_timeout, since=mark5)
        time.sleep(1)
        mark6 = con.mark()
        con.send("aethercompctl -c hash; echo COMP_HASH_FB_DONE")
        found, data = con.wait_line("COMP_HASH_FB_DONE", args.cmd_timeout,
                                    since=mark6)
        hfb = re.search(r'"hash":\s*"([0-9a-f]{16})"', data[mark6:])
        REPORT.record("J10 focus 切换后帧哈希变化（真渲染路径）",
                      bool(hfa and hfb and hfa.group(1) != hfb.group(1)),
                      f"focus {id_a} -> {hfa.group(1) if hfa else '?'} | "
                      f"focus {id_b} -> {hfb.group(1) if hfb else '?'}")
        j12_close_id = id_b   # 供 J12 关闭（-1 相对断言）

    # J11: screenshot 经 9p 落宿主，PPM 头合法且尺寸与 KMS 模式一致
    shot_guest = "/mnt/share/aether-comp-shot.ppm"
    shot_host = os.path.join(args.share, "aether-comp-shot.ppm") if args.share else ""
    mark = con.mark()
    con.send(f"aethercompctl -c screenshot {shot_guest}; echo COMP_SHOT_DONE")
    found, data = con.wait_line("COMP_SHOT_DONE", args.cmd_timeout, since=mark)
    ok_resp = re.search(r'"ok":\s*true', data[mark:]) is not None
    ppm_ok = False
    detail = "screenshot 命令未确认 ok"
    if ok_resp and shot_host:
        for _ in range(10):
            if os.path.isfile(shot_host):
                break
            time.sleep(1)
        if os.path.isfile(shot_host):
            with open(shot_host, "rb") as f:
                header = f.read(32)
            m = re.match(rb"P6\n(\d+) (\d+)\n255\n", header)
            if m:
                ppm_ok = (int(m.group(1)) == mode_w and int(m.group(2)) == mode_h)
                detail = f"PPM {m.group(1).decode()}x{m.group(2).decode()} vs mode {mode_w}x{mode_h}"
            else:
                detail = f"PPM 头非法: {header[:16]!r}"
        else:
            detail = f"宿主未见 {shot_host}（9p 同步超时）"
    REPORT.record("J11 screenshot 落盘 PPM 且尺寸=KMS 模式",
                  found and ok_resp and ppm_ok, detail)

    # J12: close 后窗口数递减（WM 真删除窗口，SystemUI 窗口不受影响）
    # P4-1：窗口基数变成「SystemUI + J10 开的两个调试窗口」= 3。关闭 J10 里
    # 打开的第二个窗口（id_b），断言恰递减 1（相对断言，不依赖基数）。
    if j12_close_id is None:
        REPORT.record("J12 close 后 windows 恰递减 1（相对断言，不依赖窗口基数）",
                      False, "J10 未取得可关闭的窗口 id")
    else:
        mark = con.mark()
        con.send("aethercompctl -c status; echo COMP_PRE_STATUS_DONE")
        found, data = con.wait_line("COMP_PRE_STATUS_DONE", args.cmd_timeout, since=mark)
        wbefore = GuestConsole.last_group(r'"windows":(\d+)', data[mark:])
        mark = con.mark()
        con.send(f"aethercompctl -c close {j12_close_id}; echo COMP_CLOSE_DONE")
        found, data = con.wait_line("COMP_CLOSE_DONE", args.cmd_timeout, since=mark)
        seg = data[mark:]
        wm = re.search(r'"windows":\s*(\d+)', seg)
        # P3-14/P4-1：SystemUI 也是 compositor 的一个客户端窗口，窗口基数不再是常量 2。
        # 断言改为「close 使窗口数恰好减 1」——比硬编码基数更严格且不受功能演进影响。
        REPORT.record("J12 close 后 windows 恰递减 1（相对断言，不依赖窗口基数）",
                      bool(wm and wbefore and int(wm.group(1)) == int(wbefore) - 1),
                      f"{wbefore} -> {wm.group(1) if wm else '?'}（期望 -1）")

    # J13: 安装的应用进程存活且由 SystemUI 监管（P4-1：demo 不再是开机服务，
    #      改由 SystemUI 经 sysui IPC `unlock` + `launch` 走沙箱启动器拉起
    #      ——与启动器点按同一链路）。进程 exe 实证来自 /data/aether/apps 安装副本。
    mark = con.mark()
    con.send("aether-sysuictl -c unlock; "
             "aether-sysuictl -c 'launch aetheros.demo'; echo SU_LAUNCH_DEMO_DONE")
    found, data = con.wait_line("SU_LAUNCH_DEMO_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    demo_launched = '"ok":true' in seg and '"id":"aetheros.demo"' in seg
    demo_pid = GuestConsole.last_group(r'"pid":(\d+)', seg)
    mark = con.mark()
    con.send("SUPID=$(ps -o pid=,comm= | awk '$2==\"aether-systemui\"{print $1}'); "
             "echo SU_PID=$SUPID; echo SU_PID_DONE")
    found, data = con.wait_line("SU_PID_DONE", args.cmd_timeout, since=mark)
    su_pid = GuestConsole.last_group(r"SU_PID=(\d+)", data[mark:])
    ppid_ok = False
    exe_line = ""
    ppid_val = "?"
    if demo_launched and demo_pid and su_pid:
        mark = con.mark()
        con.send(f"sleep 1; grep -E '^PPid:' /proc/{demo_pid}/status; "
                 f"ls -l /proc/{demo_pid}/exe; echo COMP_EXE_DONE")
        found, data = con.wait_line("COMP_EXE_DONE", args.cmd_timeout, since=mark)
        seg = data[mark:]
        ppid_val = GuestConsole.last_group(r"PPid:\s+(\d+)", seg) or "?"
        exe_line = next((l.strip() for l in seg.splitlines()
                         if "aetheros.demo" in l), "")
        # PPID 必须是 SystemUI 的 pid（launch_app fork 出来的沙箱子链同 pid），
        # 而不是 1——P4-1 起应用的生命周期归启动器管，不再是 init 直接监管。
        ppid_ok = (ppid_val == su_pid)
    REPORT.record("J13 安装的应用进程运行且由 SystemUI 监管（IPC 启动 + 沙箱子链）",
                  bool(demo_launched and demo_pid and ppid_ok and exe_line),
                  (f"demo pid={demo_pid} PPid={ppid_val} (systemui={su_pid}) "
                   f"| {exe_line}"
                   if (demo_launched and demo_pid) else
                   f"launch 失败或未取得 pid: {tail(seg)}"))

    # J14: compositor 管理由客户端注册的窗口（title=demo）
    #      P4-1：demo 由 J13 启动，窗口注册可能稍晚（TCG 慢启动）——有界轮询。
    demo_win = None
    for _ in range(15):
        mark = con.mark()
        con.send("aethercompctl -c list; echo COMP_LIST_DONE")
        found, data = con.wait_line("COMP_LIST_DONE", args.cmd_timeout, since=mark)
        seg = data[mark:]
        demo_win = re.search(r'"title":\s*"demo"', seg)
        if demo_win:
            break
        time.sleep(1)
    REPORT.record("J14 compositor 管理 demo 窗口（客户端经 IPC 注册）",
                  bool(demo_win),
                  "list 含 title=demo" if demo_win else "list 未见 demo 窗口")

    # J15: demo 像素真实上屏（截图全屏扫描红色特征块）
    #      P4-1：demo 由 J13 经 SystemUI 启动（不再是开机服务），必须在它的
    #      窗口出现后**重新截图**再扫描——J11 的截图早于 demo 存在。
    #      demo 在 content 画布绘制明亮红色方块(0xFF3030)，与玻璃/锁屏的
    #      蓝灰色调明显不同；扫描到足够红色像素即证明该独立进程的内容已
    #      经 SCM_RIGHTS 共享内存 -> compositor blit -> KMS 上屏。
    red_px = 0
    shot2_guest = "/mnt/share/aether-comp-shot2.ppm"
    shot2_host = os.path.join(args.share, "aether-comp-shot2.ppm") if args.share else ""
    if shot2_host:
        mark = con.mark()
        con.send(f"aethercompctl -c screenshot {shot2_guest}; echo COMP_SHOT2_DONE")
        found, data = con.wait_line("COMP_SHOT2_DONE", args.cmd_timeout, since=mark)
        ok_resp2 = re.search(r'"ok":\s*true', data[mark:]) is not None
        if ok_resp2:
            for _ in range(10):
                if os.path.isfile(shot2_host):
                    break
                time.sleep(1)
        if ok_resp2 and os.path.isfile(shot2_host):
            with open(shot2_host, "rb") as f:
                f.readline(); f.readline(); f.readline()  # P6 / WxH / 255
                raw = f.read()
            for i in range(0, len(raw) - 2, 3):
                r, g, b = raw[i], raw[i + 1], raw[i + 2]
                if r > 200 and g < 100 and b < 100:
                    red_px += 1
    REPORT.record("J15 demo 像素真实上屏（截图扫描红色特征块）",
                  red_px > 50,
                  f"红色特征像素 {red_px} 个（阈值 50；截图 {shot2_guest}）")

    # J16: 中文点阵字体真实上屏（P1-1b）
    #      桌面中文状态行 "AetherOS 图形合成器 窗口系统已就绪" 位于 x=40,y=H-48
    #      （横屏 800 -> 752；手机竖屏 1600 -> 1552），其中汉字部分在
    #      x∈[112,312]（文字色≈(230,237,243)近白）。
    #      扫描该矩形内的近白特征像素，证明 16x16 CJK 位图经 UTF-8 解码 ->
    #      合成器 blit -> KMS 页翻上屏（12 个汉字的字模点亮像素合计 ~959）。
    cn_px = 0
    if shot_host and os.path.isfile(shot_host):
        with open(shot_host, "rb") as f:
            f.readline(); f.readline(); f.readline()  # P6 / WxH / 255
            raw = f.read()
        ty0 = mode_h - 48
        for yy in range(ty0, ty0 + 16):
            base = yy * mode_w
            for xx in range(112, 312):
                i = (base + xx) * 3
                r, g, b = raw[i], raw[i + 1], raw[i + 2]
                if r > 180 and g > 180 and b > 180:
                    cn_px += 1
    REPORT.record("J16 中文点阵字体真实上屏（截图扫描近白汉字像素）",
                  cn_px > 400,
                  f"近白汉字特征像素 {cn_px} 个（阈值 400，字模合计~959；扫描带 y={mode_h-48}..{mode_h-32}）")


# ---------------------------------------------------------------- P1-2a evdev

def run_keyboard_checks(args, data):
    """K 段：真实 virtio 键盘接入与验证（P1-2a）。"""
    # K1: 合成器用 EVIOCGBIT 确认是真实键盘（串口日志取证）
    kbd_line = next((l.strip() for l in data.splitlines()
                     if "evdev keyboard attached" in l), "")
    REPORT.record("K1 合成器探测到真实 evdev 键盘（EVIOCGBIT 识别）",
                  bool(kbd_line), kbd_line or "未见 'evdev keyboard attached' 行")

    # K2: 经 QMP 注入真实 Tab 按键 -> 合成器焦点切换（真输入路径）
    try:
        qmp = QMP(args.log + ".qmp")
        if not qmp.handshake():
            REPORT.record("K2 QMP send-key Tab 切换焦点", False, "QMP 握手失败")
            return
    except Exception as e:
        REPORT.record("K2 QMP send-key Tab 切换焦点", False, f"QMP 连接失败: {e}")
        return

    def comp_focused():
        m0 = con.mark()
        con.send("aethercompctl -c status; echo KF_DONE")
        f0, d0 = con.wait_line("KF_DONE", args.cmd_timeout, since=m0)
        mm = re.search(r'"focused":\s*(\d+)', d0[m0:])
        return (int(mm.group(1)) if mm else None), f0

    before, _ = comp_focused()
    if before is None:
        REPORT.record("K2 QMP send-key Tab 切换焦点", False, "未取得注入前 focused")
        return
    okk = qmp.send_key("tab")
    time.sleep(2)
    after, _ = comp_focused()
    REPORT.record("K2 QMP send-key Tab 切换焦点（evdev 真输入路径）",
                  okk and after is not None and after != before,
                  f"focused {before} -> {after}（send_key={okk}）")


# ---------------------------------------------------------------- T. 触摸/指针

def run_touch_checks(args, data):
    """T 段：触摸/指针端到端（P-触摸：合成器路由点击 -> SystemUI Launcher 启动）。

    合成器持有 virtio-tablet，把命中 SystemUI 窗口的点击经 SYSUI_SOCK 的
    `tap` 命令转发，SystemUI 做 Launcher 行命中并启动应用。这里 QMP 注入一次
    点击，断言：① 合成器探测到 tablet；② SystemUI 收到 tap 并启动了应用。
    """
    # T1: 合成器用 EVIOCGBIT 确认是真实 tablet（串口日志取证）
    tab_line = next((l.strip() for l in data.splitlines()
                     if "evdev tablet attached" in l), "")
    REPORT.record("T1 合成器探测到 evdev tablet（EV_ABS + ABS_X/Y 识别）",
                  bool(tab_line), tab_line or "未见 'evdev tablet attached' 行")

    # T2: 经 QMP 注入一次点击，命中 SystemUI Launcher 第 0 行 -> 启动应用
    try:
        qmp = QMP(args.log + ".qmp")
        if not qmp.handshake():
            REPORT.record("T2 QMP 注入点击 -> Launcher 命中并启动应用",
                          False, "QMP 握手失败")
            return
    except Exception as e:
        REPORT.record("T2 QMP 注入点击 -> Launcher 命中并启动应用",
                      False, f"QMP 连接失败: {e}")
        return

    # P4-1 前置：① SystemUI 开机是锁屏态，点按只会解锁——先经 IPC 解锁；
    #           ② fs 系统层窗口只在没有普通窗口覆盖该点时收到 tap——先关掉
    #              J10/J13 留下的全部普通窗口（demo/debug 窗口）。
    mark = con.mark()
    con.send("aether-sysuictl -c unlock; echo T2_UNLOCK_DONE")
    con.wait_line("T2_UNLOCK_DONE", args.cmd_timeout, since=mark)
    mark = con.mark()
    con.send("aethercompctl -c list; echo T2_CLOSE_LIST_DONE")
    found, ldata = con.wait_line("T2_CLOSE_LIST_DONE", args.cmd_timeout, since=mark)
    closed = 0
    if found:
        for m in re.finditer(r'"id":(\d+),"title":"([^"]+)"', ldata[mark:]):
            if m.group(2) != "SystemUI":
                cmark = con.mark()
                con.send(f"aethercompctl -c close {m.group(1)}; "
                         f"echo T2_CLOSED_{m.group(1)}_DONE")
                con.wait_line(f"T2_CLOSED_{m.group(1)}_DONE", args.cmd_timeout,
                              since=cmark)
                closed += 1

    # SystemUI 是 fs 窗口（x=y=0，w/h=屏幕）：局部坐标 = 屏幕坐标。
    # Launcher 是应用网格（systemui.c grid_init，同一套公式）：
    #   cols = max(3, W//240)；tile = min(160, (W-128-(cols-1)*32)//cols)
    #   margin = (W - cols*tile - (cols-1)*32)//2
    # 首个图标块中心 = (margin + tile//2, row_top + tile//2)。
    # 竖屏 720x1600 -> (168, 280)；横屏 1280x800 -> (256, 176)。
    mark = con.mark()
    con.send("aethercompctl -c status; echo T2_STATUS_DONE")
    found, sdata = con.wait_line("T2_STATUS_DONE", args.cmd_timeout, since=mark)
    mode = re.search(r'"mode":"(\d+)x(\d+)"', sdata[mark:]) if found else None
    W = int(mode.group(1)) if mode else 1280
    H = int(mode.group(2)) if mode else 800
    row_top = 200 if H > W else 96
    gcols = max(3, W // 240)
    gtile = min(160, (W - 128 - (gcols - 1) * 32) // gcols)
    gmargin = (W - gcols * gtile - (gcols - 1) * 32) // 2
    TARGET_LX = gmargin + gtile // 2
    TARGET_LY = row_top + gtile // 2
    AXIS = 0x7FFF
    dx = TARGET_LX * AXIS // W
    dy = TARGET_LY * AXIS // H
    okp = qmp.send_pointer(dx, dy)
    time.sleep(3)

    # 点击发生在 data 捕获之后，重新读取最新日志取证
    try:
        with open(args.log, "r", errors="replace") as fh:
            fresh = fh.read()
    except Exception:
        fresh = data
    comp_line = next((l.strip() for l in fresh.splitlines()
                      if "aethercomp: tap -> SystemUI" in l), "")
    tap_line = next((l.strip() for l in fresh.splitlines()
                     if "systemui: tap (" in l and "-> launch" in l), "")
    REPORT.record("T2 QMP 注入点击 -> SystemUI Launcher 命中并启动应用",
                  okp and bool(tap_line),
                  f"closed_windows={closed}; " +
                  (tap_line or
                   (f"send_pointer={okp}; comp_route="
                    f"{comp_line or 'NONE'}; 未见 'systemui: tap (... -> launch'")))


# ---------------------------------------------------------------- P1-2b 沙箱

def run_sandbox_checks(args):
    """S 段：验证安装的应用以沙箱运行（P1-2b + P2-11）。

    读 /proc/<demo>/status，断言其为非 root（有效 uid=1000）、能力全清
    （CapEff=0）、seccomp 过滤器已装（Seccomp=2）、NoNewPrivs=1。
    P2-11 起 demo 是 .aepkg 安装的应用，由 aether-sandbox-run 施加
    aether_apply_sandbox() 后 exec——与 init 服务沙箱同一函数、同一
    组断言，只是调用方从 PID 1 变成应用运行时入口。进程 comm=demo
    （安装路径 /data/aether/apps/aetheros.demo/demo 的 basename）。"""
    mark = con.mark()
    con.send('DP=$(ps -o pid=,comm= | awk \'$2=="demo"{print $1}\'); '
             'echo SB_PID=$DP; echo SB_DONE')
    found, data = con.wait_line("SB_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    dpid = GuestConsole.last_group(r"SB_PID=(\d+)", seg)
    if not dpid:
        REPORT.record("S0 demo 应用 pid 可取得", False, "ps 未见 demo")
        return
    mark = con.mark()
    con.send("grep -E 'Uid:|CapEff:|CapPrm:|Seccomp:|NoNewPrivs:' /proc/%s/status; "
             "echo SB_STATUS_DONE" % dpid)
    found, data = con.wait_line("SB_STATUS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    if not found:
        REPORT.record("S1 demo 沙箱状态取证", False, "读取 /proc/%s/status 超时" % dpid)
        return
    # 有效 uid=1000（4 列均为 1000 表示 real/effective/saved/fs 全为非 root）
    uid1000 = re.search(r"Uid:\s+1000\s+1000\s+1000\s+1000", seg) is not None
    REPORT.record("S1 demo 以非 root uid=1000 运行（setresuid）", uid1000,
                  next((l.strip() for l in seg.splitlines() if l.startswith("Uid")),
                       "未见 Uid 行"))
    capeff0 = re.search(r"CapEff:\s+0000000000000000", seg) is not None
    REPORT.record("S2 demo 能力集已清空（CapEff=0，PR_CAPBSET_DROP+capset）",
                  capeff0,
                  next((l.strip() for l in seg.splitlines() if l.startswith("CapEff")),
                       "未见 CapEff 行"))
    sec2 = re.search(r"Seccomp:\s+2", seg) is not None
    REPORT.record("S3 demo 处于 seccomp 过滤模式（Seccomp=2，BPF 允许列表）",
                  sec2,
                  next((l.strip() for l in seg.splitlines() if l.startswith("Seccomp")),
                       "未见 Seccomp 行"))
    nnp = re.search(r"NoNewPrivs:\s+1", seg) is not None
    REPORT.record("S4 demo 置位 NoNewPrivs（阻止 setuid 逃逸）", nnp,
                  next((l.strip() for l in seg.splitlines() if l.startswith("NoNewPrivs")),
                       "未见 NoNewPrivs 行"))


# ---------------------------------------------------------------- P2-11 包管理器

def run_pkg_checks(args):
    """P 段：.aepkg 包管理器（P2-11）。

    P0/P1: 开机 pkginstall oneshot 服务真实安装了 demo.aepkg
           （list 可见 + 安装副本验签通过）
    P2:    运行时从 9p share 安装 hello.aepkg -> rc=0 且进入 list
    P3:    篡改一字节的包 -> 拒绝（SIGNATURE VERIFICATION FAILED）
    P4:    外国密钥签名的包 -> 拒绝（签名者不对，公钥钉死不是摆设）
    P5:    安装的 hello 经 aether-sandbox-run 实际运行
           （/proc/<pid>/status: uid=1000 + CapEff=0）
    """
    # P0: 开机安装的 demo.aepkg 已注册（pkginstall 服务走了真实 install 路径）
    mark = con.mark()
    con.send("aether-pkg list; echo PKG_LIST_DONE")
    found, data = con.wait_line("PKG_LIST_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    demo_in = "aetheros.demo" in seg
    REPORT.record("P0 开机安装的 demo.aepkg 已注册（pkginstall 服务）",
                  found and demo_in,
                  "list 含 aetheros.demo" if demo_in else
                  (f"list 未见 aetheros.demo; 尾部: {tail(seg)}" if found else "PKG_LIST_DONE 超时"))
    if not demo_in:
        return

    # P1: 安装时留存的签名副本可被重新验签（防"装完就坏"的回归）
    mark = con.mark()
    con.send("aether-pkg verify /data/aether/apps/aetheros.demo.aepkg; echo PKG_VERIFY_DONE")
    found, data = con.wait_line("PKG_VERIFY_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    verify_ok = "OK" in seg and "FAILED" not in seg
    REPORT.record("P1 安装副本验签通过（aether-pkg verify）",
                  found and verify_ok,
                  next((l.strip() for l in seg.splitlines() if ".aepkg" in l), "") or
                  ("" if verify_ok else f"未见 OK; 尾部: {tail(seg)}"))

    # P2: 运行时安装（9p share 传入的 hello.aepkg）
    if not args.share:
        REPORT.skip("P2 运行时安装 hello.aepkg", "未提供 --share（9p），无包可传")
        return
    mark = con.mark()
    con.send("ls /mnt/share/hello.aepkg 2>/dev/null || echo PKG_NO_SHARE_FILE; "
             "echo PKG_SHARE_LS_DONE")
    found, data = con.wait_line("PKG_SHARE_LS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    # 行级匹配：命令回显里也含 "PKG_NO_SHARE_FILE" 字样（echo 子串），
    # 整段 substring 会把回显误判为缺文件（首次实跑 P2 假 SKIP 实锤）。
    if any(l.strip() == "PKG_NO_SHARE_FILE" for l in seg.splitlines()):
        REPORT.skip("P2 运行时安装 hello.aepkg",
                    "share 中无 hello.aepkg（pkg-test-assets 阶段未构建？）")
        return
    mark = con.mark()
    con.send("cp /mnt/share/hello.aepkg /tmp/hello.aepkg && "
             "aether-pkg install /tmp/hello.aepkg; echo PKG_INSTALL_RC=$?; "
             "echo PKG_INSTALL_DONE")
    found, data = con.wait_line("PKG_INSTALL_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    rc = GuestConsole.last_group(r"PKG_INSTALL_RC=(\d+)", seg)
    installed = "installed aether.hello" in seg
    REPORT.record("P2 运行时安装 hello.aepkg（验签+解包+索引）",
                  found and rc == "0" and installed,
                  f"rc={rc}; " + ("installed aether.hello" if installed
                                  else f"尾部: {tail(seg)}"))
    if not (rc == "0" and installed):
        return
    mark = con.mark()
    con.send("aether-pkg list; echo PKG_LIST2_DONE")
    found, data = con.wait_line("PKG_LIST2_DONE", args.cmd_timeout, since=mark)
    both = "aetheros.demo" in data[mark:] and "aether.hello" in data[mark:]
    REPORT.record("P2b 安装后 list 含两个应用", found and both,
                  "aetheros.demo + aether.hello" if both else
                  (f"尾部: {tail(data, mark)}" if found else "超时"))

    # P3: 篡改一字节 -> 必须拒绝（签名覆盖全内容区）
    mark = con.mark()
    con.send("cp /tmp/hello.aepkg /tmp/hello-bad.aepkg && "
             "printf '\\xff' | dd of=/tmp/hello-bad.aepkg bs=1 seek=200 count=1 "
             "conv=notrunc 2>/dev/null; "
             "aether-pkg verify /tmp/hello-bad.aepkg; echo PKG_BAD_RC=$?; "
             "echo PKG_BAD_DONE")
    found, data = con.wait_line("PKG_BAD_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    brc = GuestConsole.last_group(r"PKG_BAD_RC=(\d+)", seg)
    refused = "SIGNATURE VERIFICATION FAILED" in seg
    REPORT.record("P3 篡改一字节的包被拒绝（签名覆盖全内容）",
                  found and brc == "1" and refused,
                  f"rc={brc}; " + ("已拒绝" if refused else f"未见拒签; 尾部: {tail(seg)}"))

    # P4: 外国密钥签名的包 -> 必须拒绝（钉死的发布公钥不是摆设）
    mark = con.mark()
    con.send("ls /mnt/share/hello-foreign.aepkg >/dev/null 2>&1 || echo PKG_NO_FOREIGN; "
             "aether-pkg verify /mnt/share/hello-foreign.aepkg; echo PKG_FRGN_RC=$?; "
             "echo PKG_FRGN_DONE")
    found, data = con.wait_line("PKG_FRGN_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    # 行级匹配（同 P2：命令回显含 "PKG_NO_FOREIGN" 字样，禁用 substring）
    if any(l.strip() == "PKG_NO_FOREIGN" for l in seg.splitlines()):
        REPORT.skip("P4 外国密钥包被拒绝", "share 中无 hello-foreign.aepkg")
    else:
        frc = GuestConsole.last_group(r"PKG_FRGN_RC=(\d+)", seg)
        frefused = "SIGNATURE VERIFICATION FAILED" in seg
        REPORT.record("P4 外国密钥签名的包被拒绝（公钥钉死）",
                      found and frc == "1" and frefused,
                      f"rc={frc}; " + ("已拒绝" if frefused
                                       else f"未见拒签; 尾部: {tail(seg)}"))

    # P5: 安装的应用经 aether-sandbox-run 真实运行且处于沙箱中
    mark = con.mark()
    con.send("aether-sandbox-run /data/aether/apps/aether.hello/hello & "
             "HP=$!; sleep 2; echo HELLO_PID=$HP; "
             "grep -E 'Uid:|CapEff:|Seccomp:' /proc/$HP/status; "
             "kill $HP 2>/dev/null; echo PKG_RUN_DONE")
    found, data = con.wait_line("PKG_RUN_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    hpid = GuestConsole.last_group(r"HELLO_PID=(\d+)", seg)
    uid1000 = re.search(r"Uid:\s+1000\s+1000\s+1000\s+1000", seg) is not None
    capeff0 = re.search(r"CapEff:\s+0000000000000000", seg) is not None
    sec2 = re.search(r"Seccomp:\s+2", seg) is not None
    tick = "aether-hello: tick" in seg
    REPORT.record("P5 安装的应用经沙箱启动器运行（tick 输出 + uid=1000 + CapEff=0 + Seccomp=2）",
                  found and bool(hpid) and tick and uid1000 and capeff0 and sec2,
                  f"pid={hpid}; tick={tick}; uid1000={uid1000}; "
                  f"capeff0={capeff0}; seccomp2={sec2}")


# ---------------------------------------------------------------- P3-14 SystemUI

def run_sysui_checks(args):
    """U 段：SystemUI 上 Linux（P3-14）。

    U1: systemui 进程 PPID=1（init 监管的系统服务）
    U2: compositor list 含 SystemUI 窗口（真客户端窗口，非合成器内置画面）
    U3: sysuictl status 报真实读数，且与客机 /proc/meminfo 实测交叉验证
    U4: Launcher 的应用网格来自包管理器真实索引（aetheros.demo + aether.hello）
    U5: IPC 启动链路：launch aether.hello -> 进程存在且以沙箱 uid=1000 运行
    U6: 键盘启动链路：QMP send-key 数字键 -> 应用被真 evdev 路径启动
    U7: 截图实证：PPM 中 SystemUI 的青色特征像素（状态条分隔线/角标）
    U8: refresh 开关生效 + uptime 读数推进（SystemUI 是活的，不是静态贴图）
    """
    # U1: 进程受 init 监管
    mark = con.mark()
    con.send("ps -o pid=,ppid=,comm= | awk '$3==\"aether-systemui\"{print $1, $2, $3}'; "
             "echo SU_PS_DONE")
    found, data = con.wait_line("SU_PS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    su_ps = re.search(r"\s*(\d+)\s+1\s+aether-systemui\s*$", seg, re.MULTILINE)
    REPORT.record("U1 systemui 进程存活且 PPID=1",
                  found and bool(su_ps),
                  su_ps.group(0).strip() if su_ps else
                  ("ps 未见 PPID=1 的 aether-systemui" if found else "SU_PS_DONE 超时"))

    # U2: 窗口在 compositor 的窗口表里
    mark = con.mark()
    con.send("aethercompctl -c list; echo SU_LIST_DONE")
    found, data = con.wait_line("SU_LIST_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    has_win = '"title":"SystemUI"' in seg
    REPORT.record("U2 compositor 窗口表含 SystemUI 窗口（真客户端窗口）",
                  found and has_win,
                  next((l.strip() for l in seg.splitlines() if "SystemUI" in l), "") or
                  ("list 未见 SystemUI" if found else "SU_LIST_DONE 超时"))

    # U3: status 真实读数 + 与 /proc 交叉验证
    mark = con.mark()
    con.send("aether-sysuictl -c status; echo SU_STATUS_DONE")
    found, data = con.wait_line("SU_STATUS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    # 单引号包裹含双引号的字面量：省掉转义，避免 shell/python 双重转义出错
    su_prefix = '{"schema":"aetheros.sysui/1"'
    st_json = next((l.strip() for l in seg.splitlines()
                    if l.strip().startswith(su_prefix)), "")
    ok_schema = '"schema":"aetheros.sysui/1"' in st_json and '"ok":true' in st_json
    REPORT.record("U3a sysuictl status 返回 aetheros.sysui/1 契约",
                  found and ok_schema,
                  st_json[:180] if st_json else ("无响应" if found else "SU_STATUS_DONE 超时"))

    su_pct = re.search(r'"mem_avail_pct":(-?\d+)', st_json)
    su_up = re.search(r'"uptime_s":([0-9.]+)', st_json)
    su_ld = re.search(r'"load":([0-9.-]+)', st_json)
    svc_r = re.search(r'"services_running":(\d+)', st_json)
    svc_t = re.search(r'"services_total":(\d+)', st_json)

    # 客机实测（同一时刻的独立读数）：MemAvailable/MemTotal 百分比
    mark = con.mark()
    con.send("awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf \"GUEST_PCT=%d\\n\", a*100/t}' "
             "/proc/meminfo; echo SU_MEM_DONE")
    found, data = con.wait_line("SU_MEM_DONE", args.cmd_timeout, since=mark)
    guest_pct = GuestConsole.last_group(r"GUEST_PCT=(-?\d+)", data[mark:])
    cross_ok = bool(su_pct and guest_pct and abs(int(su_pct.group(1)) - int(guest_pct)) <= 2)
    REPORT.record("U3b 状态条内存读数与客机 /proc/meminfo 实测一致（±2%）",
                  cross_ok,
                  f"sysui={su_pct.group(1) if su_pct else '?'}% vs "
                  f"guest={guest_pct}%" if guest_pct else "未取到客机实测")

    REPORT.record("U3c 状态条 load/uptime/services 均为真读数",
                  bool(su_ld and su_up and svc_r and svc_t and int(svc_t.group(1)) > 0),
                  f"load={su_ld.group(1) if su_ld else '?'}; "
                  f"uptime={su_up.group(1) if su_up else '?'}s; "
                  f"services={svc_r.group(1) if svc_r else '?'}/"
                  f"{svc_t.group(1) if svc_t else '?'}")

    # U3d: Android 宿主 HAL 端到端。启动前已向 9p share 写入合成快照
    #      {battery:77,charging:1}；aether-hal-android 应读为 REAL 并经
    #      SystemUI status 上报 77%。同时校验 HAL 服务进程由 init 监管。
    su_bat = re.search(r'"battery_pct":(-?\d+)', st_json)
    su_bsrc = re.search(r'"battery_src":"([A-Z]+)"', st_json)
    su_bchg = re.search(r'"battery_charging":(\d+)', st_json)
    mark = con.mark()
    con.send("ps -o pid,ppid,comm | awk '$3 ~ /^aether-hal-and/ {print $1,$2,$3}'; "
             "echo HAL_PS_DONE")
    found, data = con.wait_line("HAL_PS_DONE", args.cmd_timeout, since=mark)
    hal_ps = next((l.strip() for l in data[mark:].splitlines()
                   if "aether-hal-and" in l), "")
    hal_ok = (bool(hal_ps) and su_bsrc and su_bsrc.group(1) == "REAL"
              and su_bat and int(su_bat.group(1)) == 77
              and su_bchg and int(su_bchg.group(1)) == 1)
    REPORT.record("U3d Android 宿主 HAL：真实电量经 9p 快照 -> SystemUI 显示（REAL 77%）",
                  hal_ok,
                  f"{hal_ps or 'ps 未见 aether-hal-android'} | "
                  f"battery={su_bat.group(1) if su_bat else '?'}% "
                  f"src={su_bsrc.group(1) if su_bsrc else '?'} "
                  f"chg={su_bchg.group(1) if su_bchg else '?'}")

    # U4: 应用网格来自真实包索引（P2-11 的 /data/aether/apps/index）
    apps_all = re.findall(r'"id":"([^"]+)"', st_json)
    has_demo = "aetheros.demo" in apps_all
    REPORT.record("U4 Launcher 应用网格来自真实包索引（含 aetheros.demo）",
                  has_demo,
                  f"apps={apps_all}")

    # U5: IPC 启动链路 -> 应用以沙箱 uid=1000 运行
    has_hello = "aether.hello" in apps_all
    if not (args.share and has_hello):
        REPORT.skip("U5 IPC 启动链路", "索引中无 aether.hello（P2 运行时安装未提供）")
        REPORT.skip("U6 键盘启动链路", "同上：无第二个应用可启动")
    else:
        mark = con.mark()
        con.send("aether-sysuictl -c unlock; pkill -x hello 2>/dev/null; sleep 0.5; "
                 "aether-sysuictl -c 'launch aether.hello'; echo SU_LAUNCH_DONE")
        found, data = con.wait_line("SU_LAUNCH_DONE", args.cmd_timeout, since=mark)
        seg = data[mark:]
        launched = '"ok":true' in seg and '"id":"aether.hello"' in seg
        REPORT.record("U5a sysui IPC launch aether.hello 成功",
                      found and launched,
                      next((l.strip() for l in seg.splitlines() if '"id":' in l), "") or
                      ("无响应" if found else "SU_LAUNCH_DONE 超时"))

        # U5b: pid 取自 launch 响应，直接查 /proc（不用 ps——串口日志会被应用
        # tick 输出打断，ps 行可能被切成两半；/proc 路径不受此影响）
        hpid = GuestConsole.last_group(r'"pid":(\d+)', seg)
        mark = con.mark()
        con.send(f"sleep 1; cat /proc/{hpid}/comm 2>/dev/null; "
                 f"grep -E '^Uid:' /proc/{hpid}/status 2>/dev/null; "
                 "echo SU_HELLO_DONE")
        found, data = con.wait_line("SU_HELLO_DONE", args.cmd_timeout, since=mark)
        seg2 = data[mark:]
        is_hello = "hello" in seg2
        uid1000 = re.search(r"Uid:\s+1000\s+1000\s+1000\s+1000", seg2) is not None
        REPORT.record("U5b 启动的应用以沙箱 uid=1000 运行（sandbox-run 降权生效）",
                      found and bool(hpid) and is_hello and uid1000,
                      f"pid={hpid}; comm=hello:{is_hello}; uid1000={uid1000}")

        # U6: 键盘链路（QMP send-key 走真 evdev，与 P1-2a 同一路径）
        mark = con.mark()
        con.send("aether-sysuictl -c unlock; pkill -x hello 2>/dev/null; sleep 0.5; "
                 "echo SU_KILLHELLO_DONE")
        con.wait_line("SU_KILLHELLO_DONE", args.cmd_timeout, since=mark)
        if not args.keyboard:
            # 未附加 virtio 键盘（如 virgl 冒烟跑法）时数字键无输入设备可达——
            # 如实 SKIP，不伪造 PASS（铁律：不假实现冒充真实现）。
            REPORT.skip("U6 键盘启动链路（evdev 数字键）",
                        "未附加 virtio 键盘（--keyboard），数字键无 evdev 设备")
        else:
            try:
                qmp = QMP(args.log + ".qmp")
                ok_conn = qmp.handshake()
            except Exception:
                ok_conn = False
                qmp = None
            if not ok_conn:
                REPORT.skip("U6 键盘启动链路（evdev 数字键）", "QMP 连接失败")
            else:
                # 索引顺序由 /data/aether/apps/index 决定（pkginstall 装
                # clock/demo/settings，P2 运行时装 hello）——动态取 hello 的位次。
                idx = apps_all.index("aether.hello") + 1 if "aether.hello" in apps_all else 2
                kmark = con.mark()          # 按键前的日志水位
                okk = qmp.send_key(str(idx))
                # 证据链：数字键 -> SystemUI 的 evdev 读键 -> fork/exec 沙箱启动器。
                # 日志行由 aether-logd 转发到 console（带前缀，非 echo），用它取 pid
                kfound, _ = con.wait("systemui: digit key", 15, since=kmark,
                                     note="digit key launch")
                _, kdata = con.wait("systemui: launched", 10, since=kmark,
                                    note="launch after digit key")
                kpid = GuestConsole.last_group(
                    r"systemui: launched \S+ \(.*\) pid=(\d+)", kdata, since=kmark)
                REPORT.record("U6a 数字键经真 evdev 被 SystemUI 接收并触发启动",
                              okk and kfound and bool(kpid),
                              f"send_key={idx}; digit-key log={kfound}; pid={kpid or '?'}")
                if kpid:
                    mark = con.mark()
                    con.send(f"sleep 1; cat /proc/{kpid}/comm 2>/dev/null; "
                             f"grep -E '^Uid:' /proc/{kpid}/status 2>/dev/null; "
                             "echo SU_KBD_DONE")
                    found, data = con.wait_line("SU_KBD_DONE", args.cmd_timeout, since=mark)
                    seg = data[mark:]
                    is_hello = "hello" in seg
                    uid1000 = re.search(r"Uid:\s+1000\s+1000\s+1000\s+1000", seg) is not None
                    REPORT.record("U6b 键盘启动的应用真实运行（comm=hello + uid=1000）",
                                  found and is_hello and uid1000,
                                  f"pid={kpid}; comm=hello:{is_hello}; uid1000={uid1000}")

    # U7: 截图像素实证（SystemUI 状态条的分隔线整行上屏）
    #     真照片壁纸铺底后，全图数"青色像素"会被壁纸颜色干扰（假阳性）；
    #     改为精确匹配状态条 2px 分隔线的 FG_ACCENT(79,195,247)，且要求
    #     覆盖整宽——这一行只可能由 SystemUI 绘制。窗口遮挡（hello 窗压在
    #     分隔线上）允许损失，阈值取整宽 1 倍（两行合计 2 倍）。
    if not args.share:
        REPORT.skip("U7 状态条分隔线整行上屏", "未提供 --share，无处落盘截图")
    else:
        shot_guest = "/mnt/share/aether-sysui-shot.ppm"
        shot_host = os.path.join(args.share, "aether-sysui-shot.ppm")
        # 先关掉全部非 SystemUI 窗口：U5/U6 启动的 hello 窗压住状态条分隔线
        # 会少算几百像素。断言要的是「整行由 SystemUI 绘制」这一事实，
        # 先把遮挡物撤掉（与 T2 同一手法），阈值才能收到 ~整宽。
        # 注意：窗口明细在 `list` 里；`status` 只给"windows":N 计数。
        mark = con.mark()
        con.send("aethercompctl -c list; echo U7_LIST_DONE")
        found, sdata = con.wait_line("U7_LIST_DONE", args.cmd_timeout, since=mark)
        u7_closed = 0
        if found:
            for m in re.finditer(r'"id":(\d+),"title":"([^"]+)"', sdata[mark:]):
                if m.group(2) == "SystemUI":
                    continue
                cmark = con.mark()
                con.send(f"aethercompctl -c close {m.group(1)}; "
                         "echo U7_CLOSE_DONE")
                con.wait_line("U7_CLOSE_DONE", args.cmd_timeout, since=cmark)
                u7_closed += 1
            time.sleep(2)
        mark = con.mark()
        con.send(f"aethercompctl -c screenshot {shot_guest}; echo SU_SHOT_DONE")
        found, data = con.wait_line("SU_SHOT_DONE", args.cmd_timeout, since=mark)
        divider = 0
        sw = 0
        if found:
            time.sleep(1)
            if os.path.isfile(shot_host):
                with open(shot_host, "rb") as f:
                    f.readline()                      # P6
                    dims = f.readline().split()       # WxH
                    f.readline()                      # 255
                    raw = f.read()
                sw, sh = int(dims[0]), int(dims[1])
                bar_h = 96 if sh > sw else 48         # 竖屏手机 / 横屏基线
                for yy in (bar_h - 2, bar_h - 1):
                    base = yy * sw
                    for xx in range(sw):
                        i = (base + xx) * 3
                        if (abs(raw[i] - 79) <= 2 and abs(raw[i + 1] - 195) <= 2
                                and abs(raw[i + 2] - 247) <= 2):
                            divider += 1
        REPORT.record("U7 状态条青色分隔线整行上屏（精确色匹配，非壁纸干扰）",
                      divider >= int(sw * 1.8),
                      f"分隔线精确色像素 {divider}（整宽 {sw}，两行合计 {sw * 2}；"
                      f"先关闭 {u7_closed} 个遮挡窗口）")

    # U8: refresh 开关生效 + uptime 读数推进
    mark = con.mark()
    con.send("aether-sysuictl -c 'refresh off'; sleep 0.3; "
             "aether-sysuictl -c status | grep -o '\"refresh\":false'; "
             "aether-sysuictl -c 'refresh on'; sleep 0.3; "
             "aether-sysuictl -c status | grep -o '\"refresh\":true'; "
             "echo SU_REFRESH_DONE")
    found, data = con.wait_line("SU_REFRESH_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    off_ok = '"refresh":false' in seg
    on_ok = '"refresh":true' in seg
    REPORT.record("U8 refresh 开关双向生效（确定化测试输入用）",
                  found and off_ok and on_ok,
                  f"off={off_ok}; on={on_ok}")

    up2 = None
    mark = con.mark()
    con.send("sleep 3; aether-sysuictl -c status; echo SU_UP2_DONE")
    found, data = con.wait_line("SU_UP2_DONE", args.cmd_timeout, since=mark)
    seg2 = data[mark:]
    up2 = re.search(r'"uptime_s":([0-9.]+)', seg2)
    REPORT.record("U8b SystemUI 读数随时间推进（活的进程，非静态贴图）",
                  bool(su_up and up2 and float(up2.group(1)) > float(su_up.group(1))),
                  f"uptime {su_up.group(1) if su_up else '?'} -> "
                  f"{up2.group(1) if up2 else '?'}s")


# ---------------------------------------------------------------- P2-13a 序列化日志

def run_log_checks(args):
    """L 段：aether-logd 序列化日志服务。

    L1:  /run/aether-log.sock 存在（logd 服务在跑）
    L2:  logd 进程 PPID=1（init 监管）
    L3:  aether-logctl 客户端 -> logd -> /var/log/aether.log 全链落盘
         （一条带唯一标记的消息必须出现在日志文件里）
    L4:  落盘行格式：ISO8601 墙钟 + monotonic + LEVEL + svc[pid]
    L5:  /run/aether-services.state 存在（watchdog 的数据源）
    L6:  SIGUSR1 强制轮转 -> aether.log.1 出现（测试钩子，文档注明）
    """
    # L1/L2: 服务在线且被 init 监管
    mark = con.mark()
    con.send("ls -la /run/aether-log.sock 2>/dev/null || echo LOG_NOSOCK; "
             "ps -o pid=,ppid=,comm= | grep -E 'aether-logd' || echo LOG_NOPS; "
             "echo LOG_LS_DONE")
    found, data = con.wait_line("LOG_LS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    # 行级判否：命令回显含 "LOG_NOSOCK" 字样（echo 参数），substring 会误判
    no_sock = any(l.strip() == "LOG_NOSOCK" for l in seg.splitlines())
    has_sock = "aether-log.sock" in seg and not no_sock
    # ps -o pid=,ppid=comm= 输出三列：pid ppid comm（comm 前无额外字段）
    logd_ps = re.search(r"\s*(\d+)\s+1\s+aether-logd\s*$", seg, re.MULTILINE)
    REPORT.record("L1 /run/aether-log.sock 存在（logd 服务在线）",
                  found and has_sock,
                  next((l.strip() for l in seg.splitlines() if "aether-log.sock" in l), "") or
                  ("未见 socket" if found else "LOG_LS_DONE 超时"))
    REPORT.record("L2 logd 进程存活且 PPID=1",
                  found and bool(logd_ps),
                  logd_ps.group(0).strip() if logd_ps else
                  ("ps 未见 PPID=1 的 aether-logd" if found else "LOG_LS_DONE 超时"))

    # L3/L4: 客户端 -> 落盘全链 + 行格式
    mark = con.mark()
    con.send("aether-logctl boot-smoke info P213_LOG_CHAIN_MARKER; "
             "sleep 0.5; grep P213_LOG_CHAIN_MARKER /var/log/aether.log; "
             "echo LOG_GREP_DONE")
    found, data = con.wait_line("LOG_GREP_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    # 行级匹配：回显的命令本身含标记字样，必须找「日志文件里那一行」——
    # 日志行以 ISO8601 时间开头，回显行不会，据此区分。
    log_line = next((l for l in seg.splitlines()
                     if "P213_LOG_CHAIN_MARKER" in l
                     and re.match(r"\d{4}-\d{2}-\d{2}T", l.strip())), "")
    REPORT.record("L3 客户端 -> logd -> /var/log/aether.log 全链落盘",
                  found and bool(log_line),
                  log_line.strip() if log_line else
                  ("grep 未见标记行（落盘失败？）" if found else "LOG_GREP_DONE 超时"))
    fmt_ok = bool(re.search(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z up=\S+ INFO boot-smoke\[\d+\] P213_LOG_CHAIN_MARKER", log_line))
    REPORT.record("L4 日志行格式：ISO8601 墙钟 + monotonic + LEVEL + svc[pid]",
                  found and fmt_ok,
                  "格式符合" if fmt_ok else f"行格式不符: {log_line.strip()[:120]}")

    # L5: watchdog 的数据源文件
    mark = con.mark()
    con.send("cat /run/aether-services.state 2>/dev/null || echo SVCSTATE_NO; "
             "echo SVCSTATE_DONE")
    found, data = con.wait_line("SVCSTATE_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    no_state = any(l.strip() == "SVCSTATE_NO" for l in seg.splitlines())
    has_state = ("aetherosd" in seg and "compositor" in seg
                 and not no_state)
    REPORT.record("L5 /run/aether-services.state 含被监控服务（watchdog 数据源）",
                  found and has_state,
                  " | ".join(l.strip() for l in seg.splitlines()
                             if re.match(r"\S+ \d+ (running|done|failed|stopped)", l)) or
                  ("state 文件缺失或无监控服务" if found else "SVCSTATE_DONE 超时"))

    # L6: SIGUSR1 强制轮转（测试钩子）
    mark = con.mark()
    con.send("kill -USR1 $(ps -o pid=,comm= | awk '$2 ~ /aether-logd/{print $1}'); "
             "sleep 0.5; ls /var/log/aether.log.1 >/dev/null 2>&1 && "
             "echo LOG_ROTATED_OK || echo LOG_ROTATED_MISSING; echo LOG_ROT_DONE")
    found, data = con.wait_line("LOG_ROT_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    rotated = "LOG_ROTATED_OK" in seg
    REPORT.record("L6 SIGUSR1 强制轮转 -> aether.log.1 出现（测试钩子）",
                  found and rotated,
                  "轮转成功" if rotated else
                  ("未见 .1（日志为空时 rename ENOENT 属预期，但此前 L3 已写入）"
                   if found else "LOG_ROT_DONE 超时"))


# ---------------------------------------------------------------- P2-13c 软看门狗

def run_watchdog_checks(args):
    """W 段：aether-watchdog 软看门狗（P2-13c）。

    W1: watchdog 进程 PPID=1
    W2: 启动日志如实报告看门狗形态（QEMU 无 /dev/watchdog -> 软看门狗）
    W3: 核心价值证明：kill -STOP aetherosd（进程活着但 IPC 僵死——
        init 的 waitpid 看不见这种死法）-> watchdog 心跳超时 -> SIGKILL
        -> init 拉起新进程（新 pid != 旧 pid）
    """
    # W1/W2
    mark = con.mark()
    con.send("ps -o pid=,ppid=,comm= | grep -E 'aether-watchdog' || echo WD_NOPS; "
             "echo WD_PS_DONE")
    found, data = con.wait_line("WD_PS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    wd_ps = re.search(r"\s*(\d+)\s+1\s+aether-watchdog\s*$", seg, re.MULTILINE)
    REPORT.record("W1 watchdog 进程存活且 PPID=1",
                  found and bool(wd_ps),
                  wd_ps.group(0).strip() if wd_ps else
                  ("ps 未见 watchdog" if found else "WD_PS_DONE 超时"))

    mark = con.mark()
    # L6 已强制轮转：aether.log 可能不存在或为空——两代日志都查；
    # 直接 grep 形态行（勿 tail：形态报告在日志文件头部，tail 会截掉）
    con.send("grep -hE 'software watchdog only|hardware watchdog' "
             "/var/log/aether.log /var/log/aether.log.1 2>/dev/null | head -3; "
             "echo WD_LOG_DONE")
    found, data = con.wait_line("WD_LOG_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    soft_line = next((l.strip() for l in seg.splitlines()
                      if "software watchdog only" in l), "")
    hw_line = next((l.strip() for l in seg.splitlines()
                    if "hardware watchdog" in l and "feeding" in l), "")
    REPORT.record("W2 看门狗形态如实报告（软看门狗降级说明或硬件喂狗）",
                  found and bool(soft_line or hw_line),
                  (soft_line or hw_line)[:160] if (soft_line or hw_line) else
                  ("未见形态报告行" if found else "WD_LOG_DONE 超时"))

    # W3: STOP 冻结 -> watchdog 杀 -> init 重启
    mark = con.mark()
    con.send("OLDWD=$(ps -o pid=,comm= | awk '$2==\"aetherosd\"{print $1}'); "
             "echo WD_BEFORE_PID=$OLDWD; kill -STOP $OLDWD; echo WD_STOPPED; "
             "echo WD_STOP_DONE")
    found, data = con.wait_line("WD_STOP_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    old_pid = GuestConsole.last_group(r"WD_BEFORE_PID=(\d+)", seg)
    REPORT.record("W3a 冻结 aetherosd（SIGSTOP：进程活着但 IPC 无响应）",
                  found and bool(old_pid) and "WD_STOPPED" in seg,
                  f"pid={old_pid}" if old_pid else ("未取到 pid" if found else "超时"))

    # watchdog: 3 个周期(5s) x 5s 心跳超时 -> SIGKILL；给 90s 宽限。
    # 时序注意：H 段刚 kill -9 重启过 aetherosd，本次冻结可能落在
    # watchdog 的 120s 启动宽限窗口内（新 pid 尚未完成首次成功心跳）。
    # 宽限期不容忍「冻结」：被 SIGSTOP 的进程 /proc/stat state=T，
    # 冻结≠慢启动，照计 3-miss 处决，故 90s 窗口足够（约 20s 内处决）。
    found, data = con.wait("watchdog: killing aetherosd", 90, since=mark,
                           note="watchdog kill")
    killed = found
    REPORT.record("W3b watchdog 心跳超时后处决僵死服务（日志实证）",
                  killed,
                  "watchdog: killing aetherosd" if killed else
                  f"90s 未见处决; 尾部: {tail(data, mark)}")

    found, data = con.wait("service 'aetherosd' restarted (pid=", 60, since=mark,
                           note="init restart after watchdog kill")
    new_pid = GuestConsole.last_group(r"service 'aetherosd' restarted \(pid=(\d+)\)",
                                      data, since=mark) if found else None
    REPORT.record("W3c init 拉起重启的 aetherosd（watchdog 触发，新 pid）",
                  found and bool(new_pid),
                  f"新 pid={new_pid}" if new_pid else
                  (f"未见 restarted 行; 尾部: {tail(data, mark)}" if found else "超时"))
    if old_pid and new_pid:
        REPORT.record("W3d 新 pid != 旧 pid（真重启，watchdog 路径）",
                      old_pid != new_pid,
                      f"{old_pid} -> {new_pid}")


# ---------------------------------------------------------------- P2-13b 崩溃管理器

def run_crash_checks(args):
    """C 段：aether-crashd 崩溃管理器（P2-13b）。

    C1: crashd 进程 PPID=1 + /run/aether-crash.sock 存在
    C2: W 段/H 段的 SIGKILL 已让 init 发出崩溃事件 -> aetherosd 报告落盘
        （signal 9；崩溃管理器不是退出码台账——SIGKILL 也是异常退出）
    C3: 主动 kill -SEGV halandroid -> init 上报 -> halandroid 报告落盘（signal 11 SIGSEGV；
        P4-1 起 demo 不再是 init 服务，改杀叶子服务 halandroid）
    C4: 报告内容含诚实边界段（无 core dump / 无 /proc 快照）
    C5: halandroid 被 init 重启（restart=always 闭环）
    """
    # C1: crashd 在线
    mark = con.mark()
    con.send("ls -la /run/aether-crash.sock 2>/dev/null || echo CR_NOSOCK; "
             "ps -o pid=,ppid=,comm= | grep -E 'aether-crashd' || echo CR_NOPS; "
             "echo CR_PS_DONE")
    found, data = con.wait_line("CR_PS_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    no_sock = any(l.strip() == "CR_NOSOCK" for l in seg.splitlines())
    has_sock = "aether-crash.sock" in seg and not no_sock
    cr_ps = re.search(r"\s*(\d+)\s+1\s+aether-crashd\s*$", seg, re.MULTILINE)
    REPORT.record("C1 crashd 进程 PPID=1 且 /run/aether-crash.sock 存在",
                  found and bool(cr_ps) and has_sock,
                  (cr_ps.group(0).strip() if cr_ps else "") +
                  (" + socket" if has_sock else " + 无 socket"))

    # C2: W/H 段的 SIGKILL 产生的 aetherosd 崩溃报告
    mark = con.mark()
    con.send("ls /data/aether/crash/aetherosd-*.txt 2>/dev/null | tail -1; "
             "echo CR_LIST_DONE")
    found, data = con.wait_line("CR_LIST_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    rep = next((l.strip() for l in seg.splitlines()
                if l.strip().startswith("/data/aether/crash/aetherosd-")), "")
    REPORT.record("C2 W/H 段 SIGKILL 的崩溃报告已落盘（aetherosd）",
                  found and bool(rep),
                  rep or ("未见 aetherosd 报告" if found else "CR_LIST_DONE 超时"))
    if rep:
        mark = con.mark()
        con.send(f"grep -E 'cause:|cmdline:' '{rep}' | head -2; echo CR_CAT_DONE")
        found, data = con.wait_line("CR_CAT_DONE", args.cmd_timeout, since=mark)
        seg = data[mark:]
        sig9 = "signal 9 (SIGKILL)" in seg
        REPORT.record("C2b aetherosd 报告含 cause=signal 9 (SIGKILL)",
                      found and sig9,
                      next((l.strip() for l in seg.splitlines() if "cause:" in l), "") or
                      ("未见 cause 行" if found else "CR_CAT_DONE 超时"))

    # C3: 主动 SIGSEGV halandroid -> 报告落盘（signal 11）
    # P4-1：demo 不再是 init 服务（改由 SystemUI 启动器拉起），杀 demo 不再
    # 走 init 的监管-上报链。改杀叶子服务 halandroid（无控制台、无窗口，
    # restart=always）——崩溃管理器对上报对象的本职断言不变。
    mark = con.mark()
    con.send("HAL=$(ps -o pid=,comm= | awk '$2 ~ /^aether-hal-and/{print $1}'); "
             "echo CR_HAL_PID=$HAL; kill -SEGV $HAL; echo CR_SEGV_SENT; "
             "sleep 1.5; ls /data/aether/crash/halandroid-*.txt 2>/dev/null | tail -1; "
             "echo CR_HAL_DONE")
    found, data = con.wait_line("CR_HAL_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    hal_rep = next((l.strip() for l in seg.splitlines()
                    if l.strip().startswith("/data/aether/crash/halandroid-")), "")
    segv_sent = "CR_SEGV_SENT" in seg
    REPORT.record("C3 kill -SEGV halandroid -> init 上报 -> halandroid 崩溃报告落盘",
                  found and segv_sent and bool(hal_rep),
                  hal_rep or ("未见 halandroid 报告" if found else "CR_HAL_DONE 超时"))
    if hal_rep:
        mark = con.mark()
        con.send(f"grep -E 'cause:|cmdline:|boundaries|core dump' '{hal_rep}' | head -4; "
                 "echo CR_HALCAT_DONE")
        found, data = con.wait_line("CR_HALCAT_DONE", args.cmd_timeout, since=mark)
        seg = data[mark:]
        sig11 = "signal 11 (SIGSEGV)" in seg
        honest = "no core dump configured" in seg
        REPORT.record("C4 halandroid 报告含 cause=signal 11 (SIGSEGV) + 诚实边界段",
                      found and sig11 and honest,
                      ("SIGSEGV + 边界声明齐全" if (sig11 and honest) else
                       f"sig11={sig11}; honest={honest}"))

    # C5: halandroid 被 init 重启（restart=always 闭环）
    mark = con.mark()
    con.send("sleep 1; ps -o pid=,ppid=,comm= | awk '$3 ~ /^aether-hal-and/{print $1, $2, $3}' | head -1; "
             "echo CR_RESTART_DONE")
    found, data = con.wait_line("CR_RESTART_DONE", args.cmd_timeout, since=mark)
    seg = data[mark:]
    hal_back = re.search(r"\s*(\d+)\s+1\s+aether-hal-and\S*\s*$", seg, re.MULTILINE)
    REPORT.record("C5 halandroid 被 init 重启（崩溃 -> 恢复闭环）",
                  found and bool(hal_back),
                  hal_back.group(0).strip() if hal_back else
                  ("ps 未见 PPID=1 的 aether-hal-android" if found else "CR_RESTART_DONE 超时"))


# ---------------------------------------------------------------- P1-2e virgl

def run_virgl_checks(args, data):
    """G 段：virtio-gpu 3D/virgl 能力上报（P1-2e）。"""
    # G1: 合成器 status 上报 cap_3d（VIRTGPU_PARAM_3D_FEATURES 查询结果）
    mark = con.mark()
    con.send("aethercompctl -c status; echo VG_DONE")
    found, d = con.wait_line("VG_DONE", args.cmd_timeout, since=mark)
    seg = d[mark:]
    cap = re.search(r'"cap_3d":\s*(\d+)', seg)
    REPORT.record("G1 virgl 3D 能力上报 cap_3d=1（VIRTGPU_PARAM_3D_FEATURES）",
                  found and bool(cap) and cap.group(1) == "1",
                  f"cap_3d={cap.group(1) if cap else '?'}")
    # G2: 合成器启动日志 corroborate（真实 ioctl 取证）
    gpu_line = next((l.strip() for l in data.splitlines()
                     if "VIRTGPU_PARAM_3D_FEATURES=1" in l), "")
    REPORT.record("G2 合成器启动日志报 VIRTGPU_PARAM_3D_FEATURES=1",
                  bool(gpu_line), gpu_line or "未见 gpu caps 取证行")


if __name__ == "__main__":
    sys.exit(main())
