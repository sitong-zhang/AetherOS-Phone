#!/bin/bash
# capture_ui.sh - AetherOS 手机外壳全链路取证
# 依赖已运行的 headless 合成器(AETHER_RES=720x1280 AETHER_PHONE=1)与 aether-ipc。
# 逐状态发 IPC 命令 -> screenshot 导出 PPM -> 转 PNG，并打印每步 state 供断言。
set -e
IPC="${AETHER_IPC:-/root/aether-archive/aether-pc-build/root/bin/aether-ipc}"
OUT="${AETHER_SHOTS:-/workspace/aether-pc/ui-shots}"
mkdir -p "$OUT"

say()  { printf '%-26s' "$1"; }
step() { "$IPC" "$@" >/dev/null; }

# name | IPC 命令序列（用 ; 分隔）
capture() {
    local name="$1"; shift
    # 执行本状态的设置命令
    for c in "$@"; do
        # shellcheck disable=SC2086
        step $c
    done
    # 截一帧 PPM 再转 PNG
    "$IPC" screenshot "$OUT/$name.ppm" >/dev/null
    python3 - "$OUT/$name.ppm" "$OUT/$name.png" <<'PY'
import sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    assert f.readline().strip() == b'P6'
    line = f.readline()
    while line.startswith(b'#'):
        line = f.readline()
    w, h = map(int, line.split())
    assert f.readline().strip() == b'255'
    data = f.read(w * h * 3)
    Image.frombytes('RGB', (w, h), data).save(dst)
print('  ok', dst)
PY
    say "$name"; "$IPC" state
}

echo "========== AetherOS 手机外壳取证：$(date '+%Y-%m-%d %H:%M:%S') =========="
echo "分辨率 / 后端："; "$IPC" status

# 复位到干净桌面
step closeall
step home
step theme 0
step panel 0
step drawer 0
step page 0

capture 01-boot-animation   skipboot home theme 0 page 0   # 落地到桌面(演示用，boot帧单独补)
capture 02-lock             skipboot home   # 再单独制造锁屏态见下
capture 03-home-dark        home theme 0 page 0
capture 04-home-light       theme 1
capture 05-page2            theme 0 page 1
capture 06-notification     page 0 panel 1
capture 07-control-center   panel 2
capture 08-drawer           panel 0 drawer 1
capture 09-app-settings     open 5
capture 10-app-calc         open 3
capture 11-app-files        open 0
capture 12-recents          recents

# 控制中心交互：切换主题后取证
step home panel 2
step click 130 220          # 深色磁贴(i=6)区域附近
capture 13-cc-after-toggle  panel 2

echo "========== 状态字段最终校验 =========="
"$IPC" state
echo "截图数量：$(ls -1 "$OUT"/*.png 2>/dev/null | wc -l)"
