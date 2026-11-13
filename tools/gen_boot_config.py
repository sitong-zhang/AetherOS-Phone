#!/usr/bin/env python3
"""
从参数单一源 kernel/include/aether_kconfig.h 生成 GNU as 可用的 boot_config.h。

原因：GNU as 不识别 C 整数字面量后缀（0x1BADB002u）；但参数必须只有一份真相源，
不能手写一份汇编专用常量表（v2 §5.6 第五门 / §11.3）。此脚本做纯机械转换：
  - 仅提取 `#define NAME <纯数值>` 行（跳过字符串、表达式、注释）
  - 去掉 u/U/l/LL/ull 等后缀
  - 保留十进制/十六进制原样
生成物写入 build/kernel/boot_config.h，由 boot.S 通过 CPP 预处理包含。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "kernel", "include", "aether_kconfig.h")
DST = os.path.join(ROOT, "build", "kernel", "boot_config.h")

NUM = re.compile(r"^\s*#define\s+([A-Z_][A-Z0-9_]*)\s+(0[xX][0-9a-fA-F]+|[-+]?\d+)([uUlL]*)\s*(?:/\*.*)?$")


def main():
    if not os.path.isfile(SRC):
        print("缺少参数单一源: " + SRC, file=sys.stderr)
        return 1
    out = [
        "/* 自动生成，勿手改。真相源: kernel/include/aether_kconfig.h */",
        "#ifndef AETHER_BOOT_CONFIG_H",
        "#define AETHER_BOOT_CONFIG_H",
        "",
    ]
    n = 0
    for line in open(SRC, encoding="utf-8"):
        m = NUM.match(line.rstrip("\n"))
        if not m:
            continue
        name, val, suffix = m.group(1), m.group(2), m.group(3)
        # 汇编器不认后缀；去掉。带负号的保持一致。
        out.append(f"#define {name} {val}")
        n += 1
    out += ["", "#endif /* AETHER_BOOT_CONFIG_H */", ""]
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    open(DST, "w", encoding="utf-8").write("\n".join(out))
    print(f"已生成 {DST}（{n} 个汇编常量，来自唯一真相源）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
