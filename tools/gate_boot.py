#!/usr/bin/env python3
"""AetherOS A 轨开机门禁：从 QEMU 串口日志与真实图形帧断言整机链路。

断言只读取外部可观测证据（串口输出 + QEMU QMP screendump 抓取的首帧像素统计），
不修改验收期望、不因证据缺失而放宽判定。任何环节缺失即 FAIL。
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
    ("gpu_device",    r"fb0=yes dri=card0",                              "virtio-gpu DRM/fb 设备就绪"),
    ("compositor",    r"显示打开\s+\d+x\d+\s+backend=\d",                "自研合成器打开显示并上屏"),
    ("secure_selftest", r"AETHEROS_SECURE_READY=1",                      "安全引擎随开机自检通过"),
    ("boot_diag",     r"AETHEROS_BOOT_DIAG_DONE=1",                       "开机诊断完成"),
]
FORBIDDEN = [
    "Kernel panic", "not syncing", "Unable to mount root fs",
    "Failed to execute /init", "No working init found",
]

# 真实图形首帧判定阈值（由 tools/capture_frame.py 产出）
FRAME_MIN_COLORS = 16
FRAME_MIN_NON_BG = 0.02


def check_frame(frame_json):
    """读取 capture_frame.py 的 JSON，断言真实图形帧有效且稳定。"""
    if not frame_json or not os.path.isfile(frame_json):
        return {"passed": False, "desc": "真实图形首帧（QEMU QMP 抓帧）",
                "detail": "首帧统计 JSON 不存在"}
    with open(frame_json, "r", encoding="utf-8") as f:
        d = json.load(f)
    samples = d.get("samples", [])
    last = samples[-1] if samples else {}
    colors = last.get("sample_colors", 0)
    non_bg = last.get("non_bg_ratio", 0.0)
    valid = bool(d.get("valid")) and bool(d.get("stable"))
    pixel_ok = colors >= FRAME_MIN_COLORS and non_bg >= FRAME_MIN_NON_BG
    return {
        "passed": bool(valid and pixel_ok),
        "desc": "真实图形首帧（QEMU QMP 抓帧）",
        "detail": "valid=%s stable=%s 颜色=%s 非背景=%.3f 帧数=%s" % (
            d.get("valid"), d.get("stable"), colors, non_bg, d.get("frames")),
        "source": d.get("source"),
    }


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    # 默认证据根在仓库内 evidence/：clone 后由 boot/build.sh + boot/run-qemu.sh 现场生成，
    # 不依赖仓库外的本地归档路径（避免 clone 即断）。也可用 --log/--frame 显式指定。
    default_evid = os.path.join(repo, 'evidence', 'round-009')
    ap = argparse.ArgumentParser(description="AetherOS A 轨开机门禁机检")
    ap.add_argument("--log", default=os.path.join(default_evid, 'boot-serial.log'),
                    help="QEMU 串口日志路径")
    ap.add_argument("--frame", default=os.path.join(default_evid, 'first-frame.json'),
                    help="真实图形首帧统计 JSON（tools/capture_frame.py 产出）")
    ap.add_argument("--out", default=None, help="机检 JSON 输出路径（默认仓库内 evidence/round-009）")
    args = ap.parse_args()

    out = os.path.abspath(args.out or os.path.join(default_evid, 'gate_boot.json'))

    if not os.path.isfile(args.log):
        print(f"[gate_boot] FAIL 串口日志不存在: {args.log}")
        print("  证据为现场生成产物（不随仓库分发）。请先执行：")
        print("    bash boot/fetch-assets.sh          # 下载并校验外部资产（内核源码等）")
        print("    bash boot/build.sh                 # 5 步：内核 + 用户态 + 安全引擎 + 自建根盘")
        print("    bash boot/run-qemu.sh              # QEMU 冷启动并抓取首帧/串口证据到 evidence/")
        print("  详见 boot/README.md 的「可复现路径」。")
        result = {"passed": False, "error": "log_missing", "log": args.log,
                  "checks": {}, "forbidden_hits": [], "frame": None}
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
        frame = check_frame(args.frame)
        checks["frame"] = frame

        passed = all(v['passed'] for v in checks.values()) and not forbidden_hits
        result = {
            "passed": passed,
            "log": os.path.abspath(args.log),
            "log_bytes": os.path.getsize(args.log),
            "checks": checks,
            "forbidden_hits": forbidden_hits,
            "requirement": ("内核 → 根盘挂载 → PID 1 → 服务编排 → virtio-gpu 设备 → "
                            "合成器上屏 → 安全引擎自检 → 真实图形首帧 → 开机诊断，逐环节外部证据齐备"),
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
    fc = result.get('checks', {}).get('frame', {})
    print(f"| {fc.get('desc', '真实图形首帧')} | "
          f"{'PASS' if fc.get('passed') else 'FAIL'} | {fc.get('detail', '—')} |")
    if result.get('forbidden_hits'):
        print(f"\n[gate_boot] 致命错误命中: {result['forbidden_hits']}")
    n_pass = sum(1 for c in result.get('checks', {}).values() if c.get('passed'))
    n_total = len(result.get('checks', {}))
    print(f"\n合计：{n_pass}/{n_total} {'PASS' if result['passed'] else 'FAIL'}；JSON -> {out}")
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
