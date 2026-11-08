#!/usr/bin/env bash
# Aether VM 宿主端「在这里跑通闭环」一键脚本
# 全程只用本工程自研代码：core/vm_config.c + core/vm_runtime.c + host/aether-vm.c
set -e
R=/workspace/aether-vm
RUN=$R/out/run-v6
KERNEL=/root/aether-archive/aether-linux/dist-phone/aetheros-kernel-aarch64.Image
DISK=/root/aether-archive/aether-aarch64-build/aetheros-system-v6.qcow2

echo "=== [1/5] 编译自研虚拟机（宿主 x86_64）==="
gcc -O2 -Wall -I$R/core \
    $R/core/vm_config.c $R/core/vm_runtime.c $R/host/aether-vm.c \
    -o $R/host/aether-vm -ldl -lpthread
echo "  -> $R/host/aether-vm"

echo "=== [2/5] 准备运行目录与资产（不动原始资产）==="
rm -rf "$RUN"; mkdir -p "$RUN/frames"
cp "$KERNEL" "$RUN/kernel.img"
cp "$DISK"   "$RUN/disk.qcow2"
printf 'kernel.img %s / disk.qcow2 %s\n' "$(du -h "$RUN/kernel.img"|cut -f1)" "$(du -h "$RUN/disk.qcow2"|cut -f1)"

echo "=== [3/5] 生成注入绝对路径的配置 ==="
sed -e "s|@KERNEL@|$RUN/kernel.img|" \
    -e "s|@DISK@|$RUN/disk.qcow2|" \
    -e "s|@RUN@|$RUN|g" \
    $R/host/vm.conf > "$RUN/vm.conf"

echo "=== [4/5] 清理残留 QEMU 进程 ==="
ps -eo pid,comm | awk '$2 ~ /^qemu/ {print $1}' | while read p; do kill -9 "$p" 2>/dev/null; done || true
sleep 1

echo "=== [5/5] 用自研虚拟机真实开机并判定闭环 ==="
set +e
$R/host/aether-vm "$RUN/vm.conf" 150
VMRC=$?
set -e

echo ""
echo "=== 帧文件（9p 导出）==="
ls -lh "$RUN/frames/"*.ppm 2>/dev/null || echo "  (无 ppm)"

echo "=== 转 PNG 供查看 ==="
RUN="$RUN" python3 - <<'PY'
from PIL import Image
import glob, os
run = os.environ["RUN"]
for f in sorted(glob.glob(os.path.join(run, "frames", "*.ppm"))):
    im = Image.open(f).convert("RGB")
    px = list(im.getdata()); nb = sum(1 for r,g,b in px if r+g+b > 30)
    print(f"  {os.path.basename(f):34s} {im.size} 非黑 {nb/len(px)*100:.1f}%")
    im.save(f.replace(".ppm", ".png"))
PY

echo ""
echo "aether-vm 退出码 = $VMRC  (0=闭环跑通)"
exit $VMRC
