#!/usr/bin/env python3
"""AetherOS A 轨开机门禁：从 QEMU 串口日志断言整机链路各环节真实发生。

断言只读取可观测的串口输出，不修改验收期望、不因日志缺失而放宽判定。
"""
import argparse
import json
import os
import re
import sys

# (键, 正则, 说明)：逐环节要求串口日志出现真实证据
MARKERS = [
    ("kernel",        r"Linux version 6\.6\.70",                          "Linux 6.6.70 内核启动"),
    ("rootfs_mount",  r"(VFS: Mounted root|EXT4-fs \(vda\): mounted filesystem)",
                                                                       "自建 ext4 根盘挂载"),
    ("pid1",          r"AetherOS 启动 \(PID 1\)",                        "自研 PID 1 aether-init 启动"),
    ("services",      r"加载了\s+([1-9]\d*)\s+个服务",                  "服务清单加载且非空"),
    ("system_ready",  r"所有可启动服务已拉起，系统就绪",                  "服务编排完成"),
    ("boot_diag",     r"AETHEROS_BOOT_DIAG_DONE=1",                       "开机诊断完成"),
]
FORBIDDEN = [
    "Kernel panic", "not syncing", "Unable to mount root fs",
    "Failed to execute /init", "No working init found",
]


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    ap = argparse.ArgumentParser(description="AetherOS A 轨开机门禁机检")
    ap.add_argument("--log", default=os.path.join(
        repo, '..', 'aetheros-archive', 'evidence', 'round-007', 'boot-serial.log'),
        help="QEMU 串口日志路径")
    ap.add_argument("--out", default=None, help="机检 JSON 输出路径（默认仓库外本地归档）")
    args = ap.parse_args()

    out = os.path.abspath(args.out or os.path.join(
        repo, '..', 'aetheros-archive', 'evidence', 'round-007', 'gate_boot.json'))

    if not os.path.isfile(args.log):
        print(f"[gate_boot] FAIL 串口日志不存在: {args.log}")
        result = {"passed": False, "error": "log_missing", "log": args.log,
                  "checks": {}, "forbidden_hits": []}
    else:
        with open(args.log, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()

        checks = {}
        for key, pattern, desc in MARKERS:
            m = re.search(pattern, text)
            checks[key] = {
                "passed": bool(m),
                "desc": desc,
                "matched": m.group(0)[:120] if m else None,
            }
        forbidden_hits = [s for s in FORBIDDEN if s in text]
        passed = all(v['passed'] for v in checks.values()) and not forbidden_hits
        result = {
            "passed": passed,
            "log": os.path.abspath(args.log),
            "log_bytes": os.path.getsize(args.log),
            "checks": checks,
            "forbidden_hits": forbidden_hits,
            "requirement": "内核 → 根盘挂载 → PID 1 → 服务编排 → 开机诊断，逐环节串口证据齐备",
        }

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(result, f, ensure_ascii=False, indent=2)

    print("| 环节 | 结果 | 证据 |")
    print("|---|---|---|")
    for key, _pattern, desc in MARKERS:
        c = result.get('checks', {}).get(key, {})
        r = "PASS" if c.get('passed') else "FAIL"
        print(f"| {desc} | {r} | {c.get('matched') or '—'} |")
    if result.get('forbidden_hits'):
        print(f"\n[gate_boot] 致命错误命中: {result['forbidden_hits']}")
    print(f"\n合计：{'PASS' if result['passed'] else 'FAIL'}；JSON -> {out}")
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
