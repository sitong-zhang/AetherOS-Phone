#!/bin/sh
# boot-latest.sh —— 一条命令「装 + 跑」最新版 AetherOS。
#
# 自动探测 GitHub 最新 release（不写死版本号：将来出 phone-v4/v5 也照跑），
# 下载镜像包 -> 解包 -> 启动。未来永远拿到「最新版」。
#
# 用法（Termux，或任何带 sh/curl/tar/qemu-system-aarch64 的 Linux）：
#   curl -fsSL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/boot-latest.sh | sh
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
FALLBACK="phone-v3"   # 探测失败时的兜底版本

WORK="${AETHER_HOME:-$HOME/aetheros}"
mkdir -p "$WORK"
cd "$WORK"

echo "[aether] 工作目录: $WORK"
echo "[aether] 探测最新 release ..."
LATEST=$(curl -fsSL "$API" 2>/dev/null \
  | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1 || true)
[ -n "$LATEST" ] || { echo "[aether] 探测失败，回退 $FALLBACK"; LATEST="$FALLBACK"; }
echo "[aether] 最新版 = $LATEST"

echo "[aether] 下载 $PKG ($LATEST) ..."
curl -fL --retry 3 -o "$WORK/$PKG" "$DL/$LATEST/$PKG"

echo "[aether] 解包 ..."
tar xzf "$WORK/$PKG" -C "$WORK"

echo "[aether] 启动 AetherOS $LATEST（TCG 软件模拟，首次约 1-3 分钟）"
echo "[aether] 画面: VNC 查看器(推荐 AVNC) 连 127.0.0.1:5900 | 退出: Ctrl-A 再按 X"
echo "[aether] 交互: 数字键 1-9 或【手指点击 Launcher 应用行】启动应用"
exec sh "$WORK/phone-boot.sh"
