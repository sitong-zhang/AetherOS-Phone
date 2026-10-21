#!/bin/sh
# install-termux.sh — AetherOS 手机版一键安装并启动（Termux / Linux / macOS）
#
# 用法:
#   curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/install-termux.sh" | sh
#   sh install-termux.sh              # 本地已下载时
#   sh install-termux.sh --no-run     # 只安装不启动
#
# 下载走「直连 + 国内镜像」自动兜底（Termux 直连 github.com 被墙也能装）。
# 永远拉 releases/latest/download（最新版），不写死版本号。
set -e

REPO="sitong-zhang/AetherOS-Phone"
ASSET="aetheros-phone.tar.gz"
FALLBACK="phone-v6"
DEST="${AETHEROS_HOME:-$HOME/aetheros}"
RUN=1

case "$1" in
  --no-run) RUN=0 ;;
  -h|--help) echo "用法: sh install-termux.sh [--no-run]"; exit 0 ;;
esac

# ---- 下载器 ----
if command -v curl >/dev/null 2>&1 && curl --version >/dev/null 2>&1; then
    FETCHER="curl"
elif command -v wget >/dev/null 2>&1; then
    FETCHER="wget"
else
    echo "!! 需要 curl 或 wget，请先: pkg install curl" >&2
    exit 1
fi
echo "==> 下载器: $FETCHER"

# ---- 镜像通道 ----
MIRRORS="
https://github.com
https://gh-proxy.com/https://github.com
https://ghfast.top/https://github.com
"

fetch_rel() {
    _rel="$1"; _out="${2:-}"
    for _base in $MIRRORS; do
        _url="${_base}/${REPO}/${_rel}"
        if [ "$FETCHER" = curl ]; then
            if [ -z "$_out" ]; then
                if curl -fsSL --retry 2 --max-time 90 "$_url" 2>/dev/null; then return 0; fi
            else
                if curl -fL --retry 3 --max-time 180 -o "$_out" "$_url" 2>/dev/null; then return 0; fi
            fi
        else
            if [ -z "$_out" ]; then
                if wget -qO- --timeout=90 "$_url" 2>/dev/null; then return 0; fi
            else
                if wget -O "$_out" --timeout=180 "$_url" 2>/dev/null; then return 0; fi
            fi
        fi
        echo "    通道不可用: ${_base}" >&2
    done
    return 1
}

# ---- 探测最新 release tag（仅显示用）----
REL=$(curl -fsSL --max-time 20 "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null \
  | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1) || true
if [ -z "$REL" ]; then
    REL=$(curl -fsSL --max-time 20 "https://ghfast.top/https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null \
      | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1) || true
fi
[ -n "$REL" ] || REL="(latest)"
echo "==> 版本 = $REL"

echo "==> AetherOS 手机版一键安装"
echo "    安装目录: ${DEST}"

# ---------- 1. 安装依赖 ----------
if command -v pkg >/dev/null 2>&1; then
  echo "==> [Termux] 安装 qemu-system-aarch64-headless / curl / tar"
  pkg update -y >/dev/null 2>&1
  pkg install -y qemu-system-aarch64-headless curl tar
  pkg install -y termux-api >/dev/null 2>&1 || true
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

# ---------- 2. 下载镜像（永远最新版；失败兜底到固定版本路径）----------
mkdir -p "${DEST}"
cd "${DEST}"
echo "==> 下载镜像 (约 20MB) ..."
if ! fetch_rel "releases/latest/download/${ASSET}" "${ASSET}"; then
  echo "==> /latest/download 失败，兜底到 $FALLBACK ..."
  if ! fetch_rel "releases/download/${FALLBACK}/${ASSET}" "${ASSET}"; then
    echo "!! 所有通道都失败。" >&2
    echo "!! 试设代理: export ALL_PROXY=socks5://127.0.0.1:1080" >&2
    echo "!! 或手动下载后放 ${DEST}/${ASSET} 再重跑" >&2
    exit 1
  fi
fi
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
  echo "   3) VNC 窗口内【先点按屏幕任意处解锁】，再点按应用行(或数字键 1)启动应用"
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
