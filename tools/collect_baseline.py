#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
collect_baseline.py - AetherOS 环境基线采集（v2 §2.1）

所属层：L7 生态与工程 / 工具链
依赖：Python 3.8+ 标准库（无第三方依赖、无 root 要求、无绝对路径）
对外接口：命令行 `python3 collect_baseline.py [--out PATH] [--repo PATH]`
不变量：
  - 只读取 /proc、/sys、环境变量与仓库内文件，不写系统目录、不联网
  - 同一环境两次采集，除 generated_at 外 JSON 内容逐字节一致（§2.5 可复现）
  - 不产生任何 PASS/FAIL 门禁判定：基线只记录事实，判据归门禁脚本（§11.3 红线）
对应验收项：§2.1 环境基线、§2.5 可复现性、§15.1 环境基线表

测量方法（每个数值都有出处，§10.2）：
  arch/kernel      platform.machine() / platform.release()
  cpu_logical      os.cpu_count()
  vmx/svm          /proc/cpuinfo 全文小写后子串计数（0 表示无硬件虚拟化）
  kvm_device       os.path.exists('/dev/kvm')
  memory           /proc/meminfo 的 MemTotal / MemAvailable（kB）
  disk             shutil.disk_usage(仓库所在挂载点)
  uid/can_write_*  os.geteuid() + os.access(..., W_OK)
  toolchain        shutil.which()，返回可执行文件的绝对路径或 null
  assets           仓库内文件名匹配（不扫描仓库外路径）
"""
import argparse
import json
import os
import platform
import shutil
import sys

SCHEMA = "aether.baseline/v1"

# 需要探测的工具；值是可执行名，键是报告里的语义名
TOOLS = {
    "c_compiler": "gcc",
    "c_compiler_alt": "cc",
    "clang": "clang",
    "cross_aarch64_gcc": "aarch64-linux-gnu-gcc",
    "qemu_aarch64": "qemu-system-aarch64",
    "mkfs_ext4": "mkfs.ext4",
    "debugfs": "debugfs",
    "e2fsck": "e2fsck",
    "objcopy": "objcopy",
    "pkg_config": "pkg-config",
    "git": "git",
    "python3": "python3",
}

# 仓库内资产探测：语义名 -> 精确文件名（不扫全盘，避免机器路径依赖）
ASSET_FILES = {
    "kernel_image": ("Image", "kernel-android.img", "bzImage"),
    "system_disk": ("system.qcow2", "rootfs.ext4", "system.img"),
    "vector_font": ("aether-font.ttf",),
    "cross_config": ("build-aarch64.sh",),
}


def read_cpuinfo():
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def read_meminfo():
    out = {}
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as f:
            for line in f:
                k, _, v = line.partition(":")
                parts = v.split()
                if len(parts) >= 2 and parts[1] == "kB":
                    try:
                        out[k.strip()] = int(parts[0])
                    except ValueError:
                        pass
    except OSError:
        pass
    return out


def scan_assets(repo):
    """只遍历仓库目录树，跳过 .git 与 evidence，返回 {语义名: [相对路径]}。"""
    found = {k: [] for k in ASSET_FILES}
    wanted = {}
    for key, names in ASSET_FILES.items():
        for n in names:
            wanted[n] = key
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d not in (".git", "evidence", "__pycache__")]
        for fn in files:
            key = wanted.get(fn)
            if key:
                rel = os.path.relpath(os.path.join(root, fn), repo)
                found[key].append(rel.replace(os.sep, "/"))
    for k in found:
        found[k].sort()
    return found


def collect(repo):
    cpuinfo = read_cpuinfo()
    mem = read_meminfo()
    low = cpuinfo.lower()
    vmx, svm = low.count("vmx"), low.count("svm")
    kvm = os.path.exists("/dev/kvm")
    accel = "kvm-available" if (kvm and (vmx or svm)) else "tcg-only"

    model = ""
    for line in cpuinfo.splitlines():
        if line.lower().startswith("model name"):
            model = line.partition(":")[2].strip()
            break

    du = shutil.disk_usage(repo)
    tools = {}
    for key, exe in TOOLS.items():
        tools[key] = shutil.which(exe)

    return {
        "schema": SCHEMA,
        # 时间戳不参与可复现哈希（§2.5：时间戳除外）
        "generated_at_excluded_from_hash": True,
        "generated_at": __import__("datetime").datetime.now().isoformat(timespec="seconds"),
        "host": {
            "arch": platform.machine(),
            "kernel_release": platform.release(),
            "cpu_logical": os.cpu_count(),
            "cpu_model": model,
            "virtualization": {
                "vmx_flag_count": vmx,
                "svm_flag_count": svm,
                "kvm_device": kvm,
                "effective_mode": accel,
            },
            "memory": {
                "total_kb": mem.get("MemTotal"),
                "available_kb": mem.get("MemAvailable"),
            },
            "disk": {
                "mount_point": repo,
                "total_bytes": du.total,
                "free_bytes": du.free,
            },
            "identity": {
                "uid": os.geteuid(),
                "can_write_usr": os.access("/usr", os.W_OK),
                "can_write_run": os.access("/run", os.W_OK),
            },
        },
        "toolchain": tools,
        "assets_in_repo": scan_assets(repo),
        "notes": [
            "本文件只记录事实，不含门禁判定；判据见 §5.6 / §15（§11.3 红线）。",
            "硬件加速若将来可用，按 §0/§2.1 列为可选启用，不得写进验收前提。",
        ],
    }


def human_table(d):
    h, v = d["host"], d["host"]["virtualization"]
    m, dk, ident = h["memory"], h["disk"], h["identity"]
    rows = [
        ("项", "实测值", "出处"),
        ("架构 / 内核", f"{h['arch']} / {h['kernel_release']}", "platform"),
        ("逻辑 CPU", str(h["cpu_logical"]), "os.cpu_count()"),
        ("CPU 型号", h["cpu_model"] or "(未读到)", "/proc/cpuinfo"),
        ("vmx / svm", f"{v['vmx_flag_count']} / {v['svm_flag_count']}", "/proc/cpuinfo"),
        ("/dev/kvm", "存在" if v["kvm_device"] else "不存在", "os.path.exists"),
        ("有效模式", v["effective_mode"], "推导：kvm 且 vmx|svm>0 才可用"),
        ("内存 总/可用", f"{m['total_kb']} / {m['available_kb']} kB", "/proc/meminfo"),
        ("磁盘 总/可用", f"{dk['total_bytes']} / {dk['free_bytes']} B", "shutil.disk_usage"),
        ("uid / 可写 /usr / 可写 /run",
         f"{ident['uid']} / {int(ident['can_write_usr'])} / {int(ident['can_write_run'])}",
         "os.geteuid / os.access"),
    ]
    w = [max(len(r[i]) for r in rows) for i in range(3)]
    out = []
    for r in rows:
        out.append("| " + " | ".join(r[i].ljust(w[i]) for i in range(3)) + " |")
    out.insert(1, "|" + "|".join("-" * (x + 2) for x in w) + "|")
    tc = d["toolchain"]
    out.append("")
    out.append("工具链（null = 缺失）：")
    for k in TOOLS:
        out.append(f"  {k:<22} {tc[k] or 'null'}")
    out.append("")
    out.append("仓库内资产：")
    for k, files in d["assets_in_repo"].items():
        out.append(f"  {k:<22} {', '.join(files) if files else '(无)'}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="AetherOS 环境基线采集 v2 §2.1")
    ap.add_argument("--out", default=None, help="JSON 输出路径，默认 <repo>/evidence/baseline.json")
    ap.add_argument("--repo", default=None, help="仓库根，默认脚本上一级目录")
    ap.add_argument("--human-only", action="store_true", help="只打印人读表，不写文件")
    args = ap.parse_args()

    repo = os.path.abspath(args.repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if not os.path.isdir(repo):
        print(f"[baseline] 错误：仓库目录不存在 {repo}", file=sys.stderr)
        return 2
    d = collect(repo)

    if args.human_only:
        print(human_table(d))
        return 0

    out = os.path.abspath(args.out or os.path.join(repo, "evidence", "baseline.json"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    print(human_table(d))
    print(f"\n[baseline] JSON -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
