#!/bin/sh
# install-termux.sh — AetherOS 手机版一键安装并启动（Termux / Linux / macOS）
#
# 用法:
#   curl -sL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/install-termux.sh | sh
#   sh install-termux.sh              # 本地已下载时
#   sh install-termux.sh --no-run     # 只安装不启动
set -e

REPO="sitong-zhang/AetherOS-Phone"
REL="phone-v1"
ASSET="aetheros-phone.tar.gz"
BASE_URL="https://github.com/${REPO}/releases/download/${REL}/${ASSET}"
DEST="${AETHEROS_HOME:-$HOME/aetheros}"
RUN=1

case "$1" in
  --no-run) RUN=0 ;;
  -h|--help) echo "用法: sh install-termux.sh [--no-run]"; exit 0 ;;
esac

echo "==> AetherOS 手机版一键安装 (${REL})"
echo "    安装目录: ${DEST}"

# ---------- 1. 安装依赖 ----------
if command -v pkg >/dev/null 2>&1; then
  echo "==> [Termux] 安装 qemu-system-aarch64 / curl / tar"
  pkg update -y >/dev/null 2>&1
  pkg install -y qemu-system-aarch64 curl tar
elif command -v apt-get >/dev/null 2>&1; then
  echo "==> [Debian/Ubuntu] 安装 qemu-system-arm / curl / tar"
  sudo apt-get update -y >/dev/null 2>&1
  sudo apt-get install -y qemu-system-arm curl tar
elif command -v apk >/dev/null 2>&1; then
  echo "==> [Alpine] 安装 qemu-system-aarch64 / curl / tar"
  sudo apk add qemu-system-aarch64 curl tar
elif command -v brew >/dev/null 2>&1; then
  echo "==> [macOS] 安装 qemu / curl"
  brew install qemu
else
  echo "!! 未检测到 pkg/apt/apk/brew，请手动安装: qemu-system-aarch64, curl, tar" >&2
fi

# ---------- 2. 下载镜像 ----------
mkdir -p "${DEST}"
cd "${DEST}"
echo "==> 下载镜像 (约 20MB):"
echo "    ${BASE_URL}"
curl -fL -o "${ASSET}" "${BASE_URL}"
echo "    完成: $(du -h "${ASSET}" | cut -f1)"

# ---------- 3. 解压 ----------
echo "==> 解压..."
tar xzf "${ASSET}"
chmod +x phone-boot.sh
echo "    内容:"
ls -la | sed 's/^/      /'

# ---------- 4. 启动 ----------
if [ "${RUN}" = "1" ]; then
  echo "==> 后台启动 AetherOS ..."
  # 低内存设备默认降配，可用环境变量覆盖
  : "${AETHER_MEM:=512M}"
  : "${AETHER_SMP:=2}"
  export AETHER_MEM AETHER_SMP
  nohup sh ./phone-boot.sh > aetheros-boot.log 2>&1 &
  sleep 3
  if pgrep -f "qemu-system-aarch64" >/dev/null 2>&1; then
    echo "    QEMU 已启动 (PID $(pgrep -f 'qemu-system-aarch64' | head -1))"
  else
    echo "    !! QEMU 似乎未启动，请查看 aetheros-boot.log"
  fi
  echo ""
  echo "=================================================="
  echo " 安装完成！接下来:"
  echo "   1) 安装 AVNC (VNC 查看器) 或任何 VNC 客户端"
  echo "   2) 连接 127.0.0.1:5900"
  echo "   3) VNC 窗口内按数字键 1 启动 demo 应用"
  echo ""
  echo " 日志:   ${DEST}/aetheros-boot.log"
  echo " 停止:   在 VNC 里 Ctrl-A 后按 X，或 pkill qemu-system-aarch64"
  echo " 低内存: AETHER_MEM=384M AETHER_SMP=1 ./phone-boot.sh"
  echo "=================================================="
else
  echo ""
  echo "已安装到 ${DEST}，手动启动:"
  echo "  cd ${DEST} && ./phone-boot.sh"
fi
