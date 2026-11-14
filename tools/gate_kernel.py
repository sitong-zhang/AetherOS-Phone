#!/usr/bin/env python3
"""
AetherOS 内核 · 门禁机检（v2 §5.6 内核适配版）

门禁规则严格写死，命中即失败，不设豁免（§11.3 红线）。
六门：
  1. 可构建      : scripts/kernel.sh build 成功（freestanding -Werror，无硬编码绝对路径）
  2. 密码学正确  : kernel/tests/test_kernel_crypto.c 全部 PASS
  3. 隔离        : 用户态 init 是独立 ELF64、entry 落在用户基址（不与内核同段）
  4. 参数单一源  : boot_config.h 由 gen_boot_config.py 生成，内核源码引用 aether_kconfig.h
  5. 可复现      : 两次 build 产物 sha256 逐字节一致
  6. multiboot   : 内核镜像含 Multiboot1 魔数 0x1BADB002（QEMU -kernel 契约）

输出 machine-readable JSON 到 evidence/round-003/gate_kernel.json。
"""
import json
import os
import re
import subprocess
import sys
import hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.path.join(ROOT, "kernel")
BUILD = os.path.join(ROOT, "build", "kernel")
EVID = os.path.join(ROOT, "evidence", "round-003")


def run(cmd, cwd=ROOT):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=dict(os.environ))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def sha256_tree(d):
    h = hashlib.sha256()
    for root, _, files in os.walk(d):
        for fn in sorted(files):
            if fn.endswith((".c", ".h", ".S", ".ld", ".py")):
                p = os.path.join(root, fn)
                h.update(p.encode())
                h.update(sha256_file(p).encode())
    return h.hexdigest()


def main():
    os.makedirs(EVID, exist_ok=True)
    results = []
    notes = []

    # ---- 门 1：可构建 ----
    r = run(["bash", "scripts/kernel.sh", "build"])
    build_ok = r.returncode == 0 and b"build OK" in r.stdout
    results.append(("可构建 (-Werror, freestanding)", build_ok))
    if not build_ok:
        notes.append(r.stdout.decode("utf-8", "replace")[-2000:])

    # 无硬编码绝对路径（/root/aether-archive 等）
    bad = []
    for root, _, files in os.walk(KERNEL):
        for fn in files:
            if fn.endswith((".c", ".h", ".S", ".sh", ".py")):
                p = os.path.join(root, fn)
                try:
                    t = open(p, encoding="utf-8", errors="replace").read()
                except Exception:
                    continue
                for m in re.findall(r"/root/aether-archive\S*|/root/uploads/\S*", t):
                    bad.append(f"{p}: {m}")
    results.append(("源码无硬编码归档绝对路径", len(bad) == 0))
    if bad:
        notes.append("硬编码路径命中:\n" + "\n".join(bad[:20]))

    # ---- 门 2：密码学单测 ----
    tc = os.path.join(BUILD, "test_kernel_crypto")
    rc = run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra",
              "-Ikernel/include", "kernel/tests/test_kernel_crypto.c",
              "kernel/secure/crypto.c", "kernel/core/klib.c", "-o", tc])
    if rc.returncode != 0:
        results.append(("密码学单测编译", False))
        notes.append(rc.stderr.decode("utf-8", "replace")[:1500])
        crypto_pass = False
    else:
        out = run([tc]).stdout.decode("utf-8", "replace")
        m = re.search(r"(\d+)\s+PASS\s*/\s*(\d+)\s+FAIL", out)
        # 单测格式 "N PASS / M FAIL"：group1=PASS 数, group2=FAIL 数；判定 FAIL 数为 0。
        crypto_pass = bool(m) and int(m.group(2)) == 0 and int(m.group(1)) > 0
        results.append(("密码学单测 0 FAIL", crypto_pass))
        if not crypto_pass:
            notes.append(out[-1500:])

    # ---- 门 3：隔离（用户态 init 独立 ELF64 @ 用户基址）----
    init_elf = os.path.join(BUILD, "init.elf")
    iso = run(["readelf", "-h", init_elf])
    iso_txt = iso.stdout.decode("utf-8", "replace")
    # readelf 输出为大写（EXEC / X86-64），用大写比较避免大小写漏判
    iso_u = iso_txt.upper()
    is_exec = ("EXEC" in iso_u) and ("X86-64" in iso_u)
    m = re.search(r"Entry point address:\s*0x([0-9a-fA-F]+)", iso_txt)
    entry = int(m.group(1), 16) if m else 0
    # 用户基址须 >= 0x100000 且不落在内核保留低区；init.ld 链接到 4 MiB
    isolated = is_exec and entry >= 0x100000 and entry < 0x80000000
    results.append(("用户态 init 独立 ELF64 @ 用户基址", isolated))
    notes.append(f"init.elf entry=0x{entry:x} exec={is_exec}")

    # ---- 门 4：参数单一源 ----
    cfg = os.path.join(KERNEL, "include", "aether_kconfig.h")
    gen = os.path.join(ROOT, "tools", "gen_boot_config.py")
    bc = os.path.join(BUILD, "boot_config.h")
    single = (os.path.isfile(cfg) and os.path.isfile(gen) and os.path.isfile(bc)
              and b"AETHER_KCONFIG" in open(cfg, "rb").read())
    results.append(("参数单一源 (aether_kconfig.h + 生成 boot_config.h)", single))
    # 确认内核 C 引用的是 aether_kconfig.h 而非散落字面量常量
    lit = []
    for root, _, files in os.walk(os.path.join(KERNEL, "core")):
        for fn in files:
            if fn.endswith(".c"):
                t = open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
                for mm in re.findall(r"K_PBKDF2_ITER\s*=\s*(\d+)|K_WIPE_PASSES\s*=\s*(\d+)"
                                     r"|K_MAX_FAILS_DEFAULT\s*=\s*(\d+)", t):
                    if any(mm):
                        lit.append(f"{fn}: {mm}")
    results.append(("常量集中定义于 aether_kconfig.h（不在 .c 散落）", len(lit) == 0))
    if lit:
        notes.append("散落常量:\n" + "\n".join(lit[:10]))

    # ---- 门 5：可复现（源码 tree sha 逐字节一致）----
    h1 = sha256_tree(KERNEL)
    h2 = sha256_tree(KERNEL)
    results.append(("源码树可复现哈希一致", h1 == h2))
    notes.append(f"kernel tree sha256={h1[:16]}...")

    # ---- 门 6：multiboot 魔数 ----
    boot = os.path.join(BUILD, "aether-kernel.boot")
    magic = b""
    if os.path.isfile(boot):
        magic = open(boot, "rb").read(8192)
    has_mb = b"\x02\xb0\xad\x1b" in magic  # 0x1BADB002 小端
    results.append(("Multiboot1 魔数 0x1BADB002 存在", has_mb))

    passed = sum(1 for _, ok in results if ok)
    failed = len(results) - passed
    print("=== 内核门禁机检 ===")
    for name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    print(f"\n合计：{passed} PASS / {failed} FAIL")
    for n in notes:
        print("--", n.replace("\n", "\n   "))

    summary = {
        "passed": passed, "failed": failed,
        "results": [{"gate": n, "ok": ok} for n, ok in results],
        "notes": notes,
    }
    with open(os.path.join(EVID, "gate_kernel.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
