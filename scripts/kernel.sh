#!/usr/bin/env bash
# AetherOS 内核 · 一键脚本（v2 §2.2 六种一键的 kernel 实现）
#
#   build     从源码构建内核 + 用户态 init + 虚拟磁盘
#   rebuild   清干净重来（可复现性验证）
#   run       QEMU TCG 真启动（可加 --interactive 注入命令）
#   test      完整自动测试 + 断言 + 证据
#   evidence  导出证据四件套 + 截图
#   status    状态与产物清单
#   all       build + test + evidence
#
# 不硬编码任何 /root/aether-archive 之类的绝对路径（v2 §2 修正 #3）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
K="$ROOT/kernel"
OUT="$ROOT/build/kernel"
EVID="$ROOT/evidence/round-003"
CC="${CC:-gcc}"
QEMU="${QEMU:-qemu-system-x86_64}"

CFLAGS="-c -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-pie \
        -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -m64 \
        -fno-omit-frame-pointer -I$K/include -I$OUT"
ASPP="gcc -E -P -x c -I$K/include -I$OUT -D__ASSEMBLER__"

mkdir -p "$OUT" "$EVID"

log() { printf '[kernel] %s\n' "$*"; }
die() { printf '[kernel] ERROR: %s\n' "$*" >&2; exit 1; }

# ---------- 生成虚拟磁盘（可擦除数据区）----------
make_disk() {
    local path="$OUT/aether-data.img"
    local sectors
    sectors=$(grep -E '^#define K_DISK_TOTAL_SECTORS' "$K/include/aether_kconfig.h" | awk '{print $3}' | tr -d 'uU')
    local bytes=$((sectors * 512))
    python3 - "$path" "$bytes" <<'PY'
import sys
p, n = sys.argv[1], int(sys.argv[2])
with open(p, "wb") as f:
    f.truncate(n)   # 稀疏全零盘；内核首次 super_load 见不到 magic -> 出厂态
print("disk:", p, n, "bytes")
PY
}

# ---------- 编译 ----------
do_build() {
    log "生成汇编常量（参数单一源）"
    python3 "$ROOT/tools/gen_boot_config.py" >/dev/null || die "gen_boot_config"

    log "汇编 boot.S / isr.S（GNU as, AT&T）"
    $ASPP "$K/arch/x86_64/boot.S" -o "$OUT/boot.s" || die "boot.S 预处理"
    as --64 "$OUT/boot.s" -o "$OUT/boot.o" || die "boot.S 汇编"
    $ASPP "$K/arch/x86_64/isr.S" -o "$OUT/isr.s" || die "isr.S 预处理"
    as --64 "$OUT/isr.s" -o "$OUT/isr.o" || die "isr.S 汇编"

    log "编译用户态 init（独立 ELF64 @ 4 MiB）"
    $ASPP "$K/user/start.S" -o "$OUT/user_start.s" || die "start.S 预处理"
    as --64 "$OUT/user_start.s" -o "$OUT/user_start.o" || die "start.S 汇编"
    $CC $CFLAGS "$K/user/init.c" -o "$OUT/user_init.o" || die "用户态 init.c 编译"
    ld -n -T "$K/user/user.ld" -z max-page-size=0x1000 \
        "$OUT/user_start.o" "$OUT/user_init.o" -o "$OUT/init.elf" || die "用户态链接"
    # 嵌入内核：符号 _binary_init_elf_start/end（objcopy 由文件名生成）
    ( cd "$OUT" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        init.elf init_blob.o ) || die "initrd 嵌入"

    log "编译内核 C（freestanding, -Werror）"
    local objs=("$OUT/boot.o" "$OUT/isr.o" "$OUT/init_blob.o")
    for f in "$K"/core/*.c "$K"/secure/*.c; do
        local o="$OUT/$(basename "${f%.c}").o"
        $CC $CFLAGS "$f" -o "$o" || die "编译失败: $f"
        objs+=("$o")
    done

    log "链接 ELF64 内核 + 重框为 32 位 ELF 容器（QEMU -kernel 要求）"
    ld -n -T "$K/arch/x86_64/linker.ld" -z max-page-size=0x1000 \
        "${objs[@]}" -o "$OUT/aether-kernel.elf" || die "内核链接"
    objcopy -O elf32-i386 "$OUT/aether-kernel.elf" "$OUT/aether-kernel.boot" || die "重框"

    make_disk
    log "产物："
    ls -lh "$OUT/aether-kernel.elf" "$OUT/aether-kernel.boot" "$OUT/init.elf" "$OUT/aether-data.img"
    # 可复现哈希（只记录本次构建产物）
    ( cd "$OUT" && sha256sum aether-kernel.elf aether-kernel.boot init.elf > build-hashes.txt )
    log "build OK"
}

do_clean() {
    log "清理 $OUT（保留脚本自身需要的目录）"
    find "$OUT" -maxdepth 1 -type f ! -name 'probe_main.c' -delete 2>/dev/null || true
}

# ---------- QEMU 运行 ----------
# QEMU 用两个 socket：串口（双向日志+输入）与 QMP（screendump/quit）
do_run() {
    local serial="$OUT/qemu-serial.log"
    local qmp="$OUT/qemu-qmp.sock"
    local extra="${AETHER_QEMU_EXTRA:-}"
    rm -f "$serial" "$qmp"
    log "启动 QEMU TCG（-kernel 内置 multiboot，无 KVM）"
    timeout 180 "$QEMU" \
        -machine pc,accel=tcg -cpu max -m 512M \
        -kernel "$OUT/aether-kernel.boot" \
        -drive "file=$OUT/aether-data.img,format=raw,if=ide,media=disk" \
        -chardev "socket,id=s0,path=$OUT/qemu-serial.sock,server=on,wait=off" \
        -serial chardev:s0 \
        -qmp "unix:$qmp,server=on,wait=off" \
        -display none -no-reboot -no-shutdown \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        $extra >"$OUT/qemu.stdout" 2>"$OUT/qemu.stderr" &
    echo $! > "$OUT/qemu.pid"
    echo "$serial" > "$OUT/qemu-serial.path"
}

cmd_build()   { do_build; }
cmd_rebuild() { do_clean; do_build; }
cmd_status() {
    echo "=== AetherOS Kernel 状态 ==="
    for f in aether-kernel.elf aether-kernel.boot init.elf aether-data.img; do
        if [ -f "$OUT/$f" ]; then printf "  %-24s %s bytes\n" "$f" "$(stat -c%s "$OUT/$f")";
        else printf "  %-24s (缺失)\n" "$f"; fi
    done
    echo "  源码 C/H 文件: $(find "$K" -name '*.c' -o -name '*.h' | wc -l)"
    echo "  内核源码行数  : $(find "$K" -name '*.c' -o -name '*.h' | xargs wc -l | tail -1 | awk '{print $1}')"
    echo "  证据文件      : $(find "$EVID" -type f 2>/dev/null | wc -l)"
}
# ---- 以下命令由 Python 驱动实现（真实双向 socket + QMP 断言 + 截图）----
cmd_test()     { "$ROOT/tools/kernel_qemu_driver.py" --mode test; }
cmd_evidence() { "$ROOT/tools/kernel_qemu_driver.py" --mode evidence; }
cmd_all()      { do_build; cmd_test; cmd_evidence; }
cmd_gate()     { "$ROOT/tools/gate_kernel.py"; }

case "${1:-}" in
    build)   cmd_build ;;
    rebuild) cmd_rebuild ;;
    run)     do_run; shift; "$ROOT/tools/kernel_qemu_driver.py" "$@" ;;
    test)    cmd_test ;;
    evidence) cmd_evidence ;;
    status)  cmd_status ;;
    gate)    cmd_gate ;;
    all)     cmd_all ;;
    *)  echo "用法: $0 {build|rebuild|run|test|evidence|status|all}"; exit 2 ;;
esac
