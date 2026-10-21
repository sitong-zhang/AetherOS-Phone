#!/usr/bin/env python3
"""清除 ELF 动态段中“孤儿”的版本需求（fix-verneed）。

背景（phone-v11 真机故障）：
  bionic linker 要求 .gnu.version_r（verneed）里出现的每个 File 都必须在
  DT_NEEDED 列表中，否则直接 CANNOT LINK EXECUTABLE 退出：
    cannot find "libdw.so.1" from verneed[5] in DT_NEEDED list
  Termux 打包的 QEMU 有 libdw.so.1 / libpulse.so 的 verneed 残留——
  glibc 容忍这种不一致，Android 不容忍。

处理：凡是 verneed 引用了不在 DT_NEEDED 的库，就把该 .so 动态段里的
DT_VERSYM / DT_VERNEED / DT_VERNEEDNUM 三个条目整体移除，让 linker 按
无版本模式解析符号（这些版本信息本来就指向不存在的库，纯死引用）。

幂等：处理完的库没有 VERNEED，重跑自动跳过。
"""
import struct
import subprocess
import sys
import os

VERSYM, VERNEED, VERNEEDNUM = 0x6FFFFFF0, 0x6FFFFFFE, 0x6FFFFFFF


def read_dyn(data):
    """返回 (dynamic 段文件偏移, 条目列表)。"""
    assert data[:4] == b"\x7fELF"
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        if p_type == 2:  # PT_DYNAMIC
            p_offset = struct.unpack_from("<Q", data, off + 8)[0]
            p_filesz = struct.unpack_from("<Q", data, off + 32)[0]
            ents = [struct.unpack_from("<qQ", data, p_offset + j * 16)
                    for j in range(p_filesz // 16)]
            return p_offset, ents
    return None, None


def strip_versioning(path):
    with open(path, "r+b") as f:
        data = f.read()
        off, ents = read_dyn(data)
        if off is None:
            print(f"  {os.path.basename(path)}: 无动态段，跳过")
            return False
        kept = [e for e in ents if e[0] not in (VERSYM, VERNEED, VERNEEDNUM)]
        if len(kept) == len(ents):
            return False  # 本来就没有版本机制
        kept += [(0, 0)] * (len(ents) - len(kept))  # 用 DT_NULL 补齐原长度
        f.seek(off)
        for tag, val in kept:
            f.write(struct.pack("<qQ", tag, val))
    return True


def verneed_files(path):
    out = subprocess.run(["readelf", "-VW", path], capture_output=True, text=True).stdout
    files = set()
    for line in out.splitlines():
        ls = line.strip()
        if " File: " in ls:
            files.add(ls.split(" File: ")[1].split()[0])
    return files


def needed_files(path):
    out = subprocess.run(["readelf", "-dW", path], capture_output=True, text=True).stdout
    need = set()
    for line in out.splitlines():
        if "NEEDED" in line and "[" in line:
            need.add(line.split("[")[1].split("]")[0])
    return need


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    changed, bad = 0, 0
    for f in sorted(os.listdir(directory)):
        if not f.endswith(".so"):
            continue
        p = os.path.join(directory, f)
        orphan = verneed_files(p) - needed_files(p)
        if not orphan:
            continue
        print(f"  {f}: 清除孤儿版本需求 {sorted(orphan)}")
        if strip_versioning(p):
            changed += 1
        left = verneed_files(p) - needed_files(p) if verneed_files(p) else set()
        # 清除后 dynamic 不再引用 verneed，readelf 的版本节成为孤儿，属预期
        d = subprocess.run(["readelf", "-dW", p], capture_output=True, text=True).stdout
        if "VERNEED" in d or "VERSYM" in d:
            print(f"  FATAL: {f} 版本条目未清除干净", file=sys.stderr)
            bad += 1
    print(f"[verneed] 清理 {changed} 个库，失败 {bad} 个")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
