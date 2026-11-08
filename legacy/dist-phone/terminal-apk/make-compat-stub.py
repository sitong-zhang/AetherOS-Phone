#!/usr/bin/env python3
"""生成并编译 libaether-compat.so（符号补齐 stub）。

背景（phone-v12 真机故障）：
  bionic 加载可执行文件时会**急切解析全部 UND 符号**，缺一个就
  CANNOT LINK EXECUTABLE。Termux 的 QEMU 编译时链接了 alsa/jack/
  pulseaudio/libusb/spice/usbredir/libssh/curl/libdw 等可选库，我们
  只用 VNC + virtio，这些功能根本不会触发，但这些符号必须有人提供。

做法：
  1. 从 QEMU 的 dynsym 抠出全部 UND 符号，减去包内其他库导出的；
  2. 只保留“确定来自未打包的可选库”的前缀（见 PREFIXES）；
  3. 每个符号生成一个空函数（OBJECT 型符号生成 8 字节零数据），
     编成 aarch64 的 libaether-compat.so（-nostdlib，无任何依赖）；
  4. patchelf 把它加进 QEMU 的 DT_NEEDED。

幂等：每次根据当前 QEMU 重新生成。
"""
import re
import subprocess
import sys
import os

LIBDIR = sys.argv[1] if len(sys.argv) > 1 else "lib/arm64-v8a"
QEMU = os.path.join(LIBDIR, "libqemu-system-aarch64.so")
OUT = os.path.join(LIBDIR, "libaether-compat.so")

# 只 stub 这些“来自未打包可选库”的前缀，避免误伤 bionic 的 malloc/pthread 等
PREFIXES = (
    "dwfl_", "pa_", "jack_", "libusb_", "spice_", "usbredir",
    "ssh_", "sftp_", "snd_", "curl_",
)


def strip_ver(s):
    return s.split("@")[0]


def dynsyms(path):
    out = subprocess.run(["readelf", "-sW", "--dyn-syms", path],
                         capture_output=True, text=True).stdout
    syms = []  # (name, type, bind, shndx)
    for line in out.splitlines():
        m = re.search(r"\s(\w+)\s+(\w+)\s+(FUNC|IFUNC|OBJECT)\s+(GLOBAL|WEAK)\s+DEFAULT\s+(\S+)\s+(\S+)$", line)
        if m:
            syms.append((strip_ver(m.group(6)), m.group(3), m.group(4), m.group(5)))
    return syms


def main():
    qemu_syms = dynsyms(QEMU)
    und = {n: (t, b) for n, t, b, s in qemu_syms if s == "UND"}

    exported = set()
    for f in sorted(os.listdir(LIBDIR)):
        if not f.endswith(".so") or f == os.path.basename(QEMU) or f == os.path.basename(OUT):
            continue  # 排除 QEMU（分析对象）和上次的 stub 自身（避免误判“已满足”而被空库覆盖）
        for n, t, b, s in dynsyms(os.path.join(LIBDIR, f)):
            if s != "UND":
                exported.add(n)

    missing = sorted(n for n in und if n not in exported and n.startswith(PREFIXES))
    skipped = sorted(n for n in und if n not in exported and not n.startswith(PREFIXES))
    print(f"[compat] QEMU UND 符号 {len(und)} 个，其中 {len(skipped)} 个由 bionic 提供，"
          f"{len(missing)} 个需要 stub")

    c = ["/* 自动生成：make-compat-stub.py —— 空实现，仅为满足 bionic 急切符号解析 */",
         "typedef long L;"]
    for n in missing:
        typ = und[n][0]
        safe = re.sub(r"\W", "_", n)
        if typ == "OBJECT":
            c.append(f"L {n}; /* 数据对象：8 字节零，读出即 NULL 终止 */")
        else:
            c.append(f"void {n}(void) {{ }} /* {typ} 空实现 */")
    src = "\n".join(c) + "\n"

    # .c 源文件不能留在 lib/arm64-v8a/ 里（会被整目录打进 APK，触发库名自检失败）
    cfile = os.path.join(os.path.dirname(LIBDIR.rstrip("/")) or ".", "compat-stub.c")
    with open(cfile, "w") as f:
        f.write(src)

    cc = "aarch64-linux-gnu-gcc"
    if not subprocess.run(["which", cc], capture_output=True).returncode == 0:
        print("FATAL: 缺少 aarch64-linux-gnu-gcc（apt install gcc-aarch64-linux-gnu）", file=sys.stderr)
        sys.exit(1)
    r = subprocess.run([cc, "-shared", "-nostdlib", "-fPIC", "-O1",
                        "-Wl,-soname,libaether-compat.so",
                        "-Wl,-z,max-page-size=16384",
                        "-o", OUT, cfile], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        sys.exit(1)

    # 挂进 QEMU 的依赖；stub 自身也统一 RUNPATH=$ORIGIN（构建校验一刀切的要求）
    subprocess.run(["patchelf", "--set-soname", "libaether-compat.so", OUT], check=True)
    subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", OUT], check=True)
    d = subprocess.run(["readelf", "-dW", QEMU], capture_output=True, text=True).stdout
    if "libaether-compat.so" not in d:
        subprocess.run(["patchelf", "--add-needed", "libaether-compat.so", QEMU], check=True)

    # 终检：QEMU 的 UND 减去包内导出 + stub 导出，必须为空
    stub_exp = {n for n, t, b, s in dynsyms(OUT) if s != "UND"}
    still = sorted(n for n in und if n not in exported and n not in stub_exp
                   and n.startswith(PREFIXES))
    if still:
        print("FATAL: 仍有未补齐的符号: " + ", ".join(still), file=sys.stderr)
        sys.exit(1)
    print(f"[compat] libaether-compat.so 已生成：stub {len(missing)} 个符号，"
          f"QEMU NEEDED 已挂接")


if __name__ == "__main__":
    main()
