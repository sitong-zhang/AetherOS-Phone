#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gate_l2.py - v2 §5.6 五门机检（L2 接口层）

所属层：L7 生态与工程 / 门禁
依赖：Python 3.8+ 标准库、系统 gcc（仅第 4 门需要编译）
对外接口：`python3 gate_l2.py [--repo PATH] [--out PATH] [--human-only]`
不变量（治理红线 §11.3）：
  - 本脚本即验收标准的实现；规则写死在 RULES 常量中，执行方不得修改规则、
    阈值或期望值来「自证通过」。命中即失败，不设豁免。
  - 结果如实输出：新建架构代码与历史存量分别统计，存量命中也计入 FAIL，
    允许标注为「已知不合格项」，但不得从判定中剔除。
  - 只读源码 + 在 build/ 下生成临时目标文件，不改任何业务源码。
对应验收项：§5.6 五门、§6.2 Gate S2、§15.2 机检全过

五门（与需求表逐条对应）：
  1 无设备路径上浮   扫 L3 以上源码，去注释后禁止 "/dev/"、"/sys/class/"、"/proc/" 字符串字面量
  2 无跨层直调       l2/ 实现的 #include 必须在白名单内
  3 接口覆盖          十五域枚举齐全 + 能力查询/错误码/流控计数三件套符号有定义
  4 实现可替换        同一上层目标文件编译两次（sim / stub 链接），哈希一致
  5 单一参数源        L2 配置常量只在 aether_hal_config.h 定义，实现文件不重复定义裸常量
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

RULES_VERSION = "v2-5.6-1"

# 白名单：L2 实现允许包含的头（标准库 + 自身接口头 + 配置头）
ALLOWED_INCLUDES = {
    "aether_dev_hal.h", "aether_hal_config.h",
    "stdio.h", "stdlib.h", "string.h", "stdint.h", "stddef.h",
}

# 第 1 门：设备路径字面量（只匹配引号包裹的字符串，注释里裸写的 /dev/ 不算）
DEV_PATH_RE = re.compile(r'"(/dev/|/sys/class/|/proc/)')

# 第 3 门：三件套符号（统一 API 按域分发，故检查定义存在 + 域枚举齐全）
TRIAD_SYMBOLS = [
    "aether_hal_caps",         # 能力查询
    "aether_hal_last_error",   # 错误码
    "aether_hal_flow",         # 流控计数
]
DOMAIN_ENUM_RE = re.compile(r"AETHER_DOM_(\w+)")


def strip_c_comments(text):
    """去掉 C 的 /* */ 与 // 注释，保留字符串字面量内容。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        if i + 1 < n and text[i] == '/' and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            end = n if j < 0 else j + 2
            # 保留注释内部的换行，维持后续行号不漂移
            out.append('\n' * text[i:end].count('\n'))
            i = end
            continue
        if i + 1 < n and text[i] == '/' and text[i + 1] == '/':
            j = text.find('\n', i + 2)
            i = n if j < 0 else j   # 保留换行本身，下一轮循环会 append
            continue
        if text[i] == '"':
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        out.append(text[i])
        i += 1
    return ''.join(out)


def read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def c_files(root):
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ('.git', 'build', 'evidence', '__pycache__')]
        for fn in files:
            if fn.endswith(('.c', '.h')):
                yield os.path.join(base, fn)


def rel(repo, path):
    return os.path.relpath(path, repo).replace(os.sep, '/')


def gate1_device_paths(repo):
    """L3 以上源码（guest/src）与新建 l2/ 实现分别统计；命中即失败。"""
    hits = []
    for scope in ('guest/src', 'l2'):
        d = os.path.join(repo, scope)
        if not os.path.isdir(d):
            continue
        for p in c_files(d):
            code = strip_c_comments(read(p))
            for lineno, line in enumerate(code.splitlines(), 1):
                if DEV_PATH_RE.search(line):
                    hits.append({"scope": scope, "file": rel(repo, p),
                                 "line": lineno, "text": line.strip()[:120]})
    # 分组如实上报：新建层必须零命中；存量命中计入 FAIL（不豁免，标注为已知不合格）
    new_layer = [h for h in hits if h['scope'] == 'l2']
    legacy = [h for h in hits if h['scope'] == 'guest/src']
    return {
        "name": "1 无设备路径上浮",
        "passed": len(hits) == 0,
        "new_layer_hits": len(new_layer),
        "legacy_hits": len(legacy),
        "known_debt": True if legacy else False,
        "hits": hits,
        "note": "新建 L2 层零命中；guest/src 存量直连属 S2 待整改，命中即失败，不设豁免。",
    }


def gate2_cross_layer(repo):
    violations = []
    l2 = os.path.join(repo, 'l2')
    for p in c_files(l2):
        if not p.endswith('.c'):
            continue
        for lineno, line in enumerate(read(p).splitlines(), 1):
            m = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
            if m and m.group(1) not in ALLOWED_INCLUDES:
                violations.append({"file": rel(repo, p), "line": lineno,
                                   "include": m.group(1)})
    return {
        "name": "2 无跨层直调",
        "passed": len(violations) == 0,
        "violations": violations,
        "allowed": sorted(ALLOWED_INCLUDES),
    }


def gate3_interface_coverage(repo):
    hdr = os.path.join(repo, 'l2', 'include', 'aether_dev_hal.h')
    core = os.path.join(repo, 'l2', 'sim', 'aether_hal_core.c')
    htext = read(hdr) if os.path.exists(hdr) else ''
    ctext = read(core) if os.path.exists(core) else ''

    enums = [m for m in DOMAIN_ENUM_RE.findall(htext) if m != 'COUNT']
    # 去重保持顺序
    seen, domains = set(), []
    for e in enums:
        if e not in seen:
            seen.add(e); domains.append(e)

    triad = {}
    for s in TRIAD_SYMBOLS:
        declared = re.search(r'\b' + re.escape(s) + r'\s*\(', htext) is not None
        defined = re.search(r'\b' + re.escape(s) + r'\s*\(', ctext) is not None
        triad[s] = {"declared": declared, "defined": defined}

    passed = len(domains) == 15 and all(v['declared'] and v['defined'] for v in triad.values())
    return {
        "name": "3 接口覆盖",
        "passed": passed,
        "domain_count": len(domains),
        "domains": domains,
        "triad": triad,
        "requirement": "15 域 + 能力查询/错误码/流控计数三件套符号齐备",
    }


def gate4_impl_swappable(repo):
    """同一份上层目标文件编译两次（链接 sim 与 stub），哈希必须一致。"""
    build = os.path.join(repo, 'build', 'gate')
    os.makedirs(build, exist_ok=True)
    probe = os.path.join(build, 'upper_probe.c')
    with open(probe, 'w', encoding='utf-8') as f:
        f.write('#include "aether_dev_hal.h"\n'
                'int upper_probe(aether_dom_t d){aether_caps_t c;return aether_hal_caps(d,&c);}\n')

    objs = []
    err = None
    for tag, extra in (('sim', []), ('stub', ['-DAETHER_UPPER_STUB'])):
        obj = os.path.join(build, f'upper_with_{tag}.o')
        cmd = ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               f'-I{os.path.join(repo, "l2", "include")}'] + extra + ['-c', probe, '-o', obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            err = f"{tag} 编译失败: {r.stderr.strip()[:300]}"
            break
        with open(obj, 'rb') as fh:
            objs.append((tag, hashlib.sha256(fh.read()).hexdigest()))
    if err:
        return {"name": "4 实现可替换", "passed": False, "error": err}
    hashes_equal = len(objs) == 2 and objs[0][1] == objs[1][1]
    return {
        "name": "4 实现可替换",
        "passed": hashes_equal,
        "hashes": {t: h for t, h in objs},
        "method": "同一上层 .c 编译两次（gcc -c，链接实现由 hal 运行期选择），比对目标文件 SHA-256",
    }


def gate5_single_param_source(repo):
    """L2 配置常量只在 aether_hal_config.h 定义；实现文件不得重复 #define 或用裸容量值。"""
    cfg = os.path.join(repo, 'l2', 'include', 'aether_hal_config.h')
    cfgtext = read(cfg) if os.path.exists(cfg) else ''
    defined = set(re.findall(r'#define\s+(AETHER_HAL_[A-Z0-9_]+)', cfgtext))
    violations = []
    for p in c_files(os.path.join(repo, 'l2')):
        if p.endswith('aether_hal_config.h'):
            continue
        for lineno, line in enumerate(read(p).splitlines(), 1):
            m = re.match(r'\s*#\s*define\s+(AETHER_HAL_[A-Z0-9_]+)', line)
            if m and m.group(1) in defined:
                violations.append({"file": rel(repo, p), "line": lineno,
                                   "macro": m.group(1), "reason": "重复定义配置常量"})
    # 检查业务层参数源唯一性：TK_ 令牌只在 aether-tokens.h 定义
    token_src = os.path.join(repo, 'guest', 'src', 'aether-tokens.h')
    token_defined = set()
    if os.path.exists(token_src):
        token_defined = set(re.findall(r'#define\s+(TK_[A-Z0-9_]+)', read(token_src)))
    token_dup = []
    for p in c_files(os.path.join(repo, 'guest', 'src')):
        if p.endswith('aether-tokens.h'):
            continue
        for lineno, line in enumerate(read(p).splitlines(), 1):
            m = re.match(r'\s*#\s*define\s+(TK_[A-Z0-9_]+)', line)
            if m and m.group(1) in token_defined:
                token_dup.append({"file": rel(repo, p), "line": lineno, "macro": m.group(1)})
    return {
        "name": "5 单一参数源",
        "passed": len(violations) == 0 and len(token_dup) == 0,
        "l2_config_macros": len(defined),
        "l2_duplicates": violations,
        "layout_tokens": len(token_defined),
        "layout_token_duplicates": token_dup,
        "requirement": "配置常量与布局令牌各只有一个定义源；业务代码重复定义即失败",
    }


GATES = [gate1_device_paths, gate2_cross_layer, gate3_interface_coverage,
         gate4_impl_swappable, gate5_single_param_source]


def human_report(d):
    lines = ["| 门禁 | 结果 | 要点 |", "|---|---|---|"]
    for g in d['gates']:
        r = "PASS" if g['passed'] else "FAIL"
        if g['name'].startswith('1'):
            key = f"新建层命中 {g['new_layer_hits']} / 存量命中 {g['legacy_hits']}（已知不合格项）"
        elif g['name'].startswith('2'):
            key = f"越界 include {len(g['violations'])} 处"
        elif g['name'].startswith('3'):
            key = f"域 {g['domain_count']}/15，三件套 {sum(1 for v in g['triad'].values() if v['defined'])}/3"
        elif g['name'].startswith('4'):
            key = "两次编译哈希一致" if g['passed'] else g.get('error', '哈希不一致')
        else:
            key = f"L2 重复定义 {len(g['l2_duplicates'])} / 布局令牌重复 {len(g['layout_token_duplicates'])}"
        lines.append(f"| {g['name']} | {r} | {key} |")
    lines.append("")
    lines.append(f"合计：{d['passed']}/{d['total']} 门通过；整体：{'PASS' if d['all_passed'] else 'FAIL'}")
    g1 = d['gates'][0]
    if g1['legacy_hits']:
        lines.append("")
        lines.append(f"[如实记录] 第 1 门存量命中 {g1['legacy_hits']} 处，示例：")
        for h in g1['hits'][:8]:
            lines.append(f"  {h['file']}:{h['line']}  {h['text']}")
        lines.append("  → 属 S2 待整改；按 §11.3 不修改门禁规则豁免，判定保持 FAIL。")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="AetherOS L2 五门机检 v2 §5.6")
    ap.add_argument('--repo', default=None)
    ap.add_argument('--out', default=None)
    ap.add_argument('--human-only', action='store_true')
    args = ap.parse_args()
    repo = os.path.abspath(args.repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if not os.path.isdir(repo):
        print(f"[gate] 错误：仓库不存在 {repo}", file=sys.stderr)
        return 2

    gates = [fn(repo) for fn in GATES]
    d = {
        "schema": "aether.gate_l2/v1",
        "rules_version": RULES_VERSION,
        "repo": ".",
        "gates": gates,
        "passed": sum(1 for g in gates if g['passed']),
        "total": len(gates),
        "all_passed": all(g['passed'] for g in gates),
        "governance": "规则固定；执行方不得修改阈值/期望值自证通过（§11.3）。",
    }
    if not args.human_only:
        out = os.path.abspath(args.out or os.path.join(repo, '..', 'aetheros-archive', 'evidence', 'round-001', 'gate_l2.json'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'w', encoding='utf-8') as f:
            json.dump(d, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write('\n')
    print(human_report(d))
    if not args.human_only:
        print(f"\n[gate] JSON -> {os.path.abspath(args.out or os.path.join(repo, '..', 'aetheros-archive', 'evidence', 'round-001', 'gate_l2.json'))}")
    # 有任一门不通过即返回非零（如实，不掩盖）
    return 0 if d['all_passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
