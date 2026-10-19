#!/bin/sh
# boot-latest.sh —— 一条命令「装 + 跑」最新版 AetherOS。
#
# 自动探测 GitHub 最新 release（不写死版本号：将来出 phone-v4/v5 也照跑），
# 下载镜像包 -> 解包 ->（缺 QEMU 则自动装）-> 启动。永远拿「最新版」。
#
# 用法（Termux，或任何带 sh + curl/wget + tar + qemu-system-aarch64 的 Linux）：
#   curl -fsSL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/boot-latest.sh | sh
#   # curl 坏时用 wget 版：
#   wget -qO- https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/boot-latest.sh | sh
#
# 可选环境变量：
#   AETHER_MEM=384M   低内存手机（3GB 以下）调小内存
#   AETHER_SMP=1      单 vCPU，TCG 软件模拟下更稳
#   AETHER_HOME=DIR   安装/运行目录（默认 ~/aetheros）
set -eu

REPO="sitong-zhang/AetherOS-Phone"
DL="https://github.com/$REPO/releases/download"
API="https://api.github.com/repos/$REPO/releases/latest"
PKG="aetheros-phone.tar.gz"
FALLBACK="phone-v4"   # 探测失败时的兜底版本
WORK="${AETHER_HOME:-$HOME/aetheros}"

# ---- 下载器：curl 优先，坏/缺则 wget 兜底 ----
# 注意：Termux 里 curl 可能因 libssl 版本不匹配而 CANNOT LINK——真正跑一次
# --version 才知道能不能用（command -v 只证明二进制存在，不证明能启动）。
if command -v curl >/dev/null 2>&1 && curl --version >/dev/null 2>&1; then
    GET()  { curl -fsSL "$1"; }                 # 内容到 stdout（探测版本号）
    DLTO() { curl -fL --retry 3 -o "$2" "$1"; } # 下载到文件
    FETCHER="curl"
elif command -v wget >/dev/null 2>&1; then
    GET()  { wget -qO- "$1"; }
    DLTO() { wget -O "$2" "$1"; }
    FETCHER="wget"
else
    echo "[aether] 需要 curl 或 wget，请先: pkg install curl"
    exit 1
fi
echo "[aether] 下载器: $FETCHER"

mkdir -p "$WORK"
cd "$WORK"
echo "[aether] 工作目录: $WORK"

echo "[aether] 探测最新 release ..."
LATEST=$(GET "$API" 2>/dev/null \
  | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1 || true)
[ -n "$LATEST" ] || { echo "[aether] 探测失败，回退 $FALLBACK"; LATEST="$FALLBACK"; }
echo "[aether] 最新版 = $LATEST"

echo "[aether] 下载 $PKG ($LATEST) ..."
DLTO "$DL/$LATEST/$PKG" "$WORK/$PKG"

echo "[aether] 解包 ..."
tar xzf "$WORK/$PKG" -C "$WORK"

# ---- QEMU：缺失则自动安装（Termux=apt / Debian=apt-get）----
if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[aether] 未检测到 QEMU，自动安装（较大，首次约几分钟）..."
    if command -v pkg >/dev/null 2>&1; then
        pkg install -y qemu-system-aarch64-headless || true
    elif command -v apt-get >/dev/null 2>&1; then
        apt-get update -y >/dev/null 2>&1 || true
        apt-get install -y qemu-system-arm64 2>/dev/null || \
            apt-get install -y qemu-system-aarch64 || true
    fi
    if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
        echo "[aether] 自动安装 QEMU 失败，请手动: pkg install qemu-system-aarch64-headless"
        echo "[aether] 装好后重新跑: sh $WORK/phone-boot.sh"
        exit 1
    fi
fi

echo "[aether] 启动 AetherOS $LATEST（TCG 软件模拟，首次约 1-3 分钟）"
echo "[aether] 画面: VNC 查看器(推荐 AVNC) 连 127.0.0.1:5900 | 退出: Ctrl-A 再按 X"
echo "[aether] 交互: 数字键 1-9 或【手指点击 Launcher 应用行】启动应用"
exec sh "$WORK/phone-boot.sh"
