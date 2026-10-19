#!/bin/sh
# install-termux.sh — AetherOS 手机版一键安装并启动（Termux / Linux / macOS）
#
# 用法:
#   curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/install-termux.sh | sh
#   sh install-termux.sh              # 本地已下载时
#   sh install-termux.sh --no-run     # 只安装不启动
#
# 注意：用 github.com 域名而不是 raw.githubusercontent.com——后者在国内
# 网络常被连接重置（curl: (35) Recv failure: Connection reset by peer）。
set -e

REPO="sitong-zhang/AetherOS-Phone"
API="https://api.github.com/repos/${REPO}/releases/latest"
ASSET="aetheros-phone.tar.gz"
FALLBACK="phone-v5"   # 探测失败时的兜底版本（诚实降级：可能不是最新版）
DEST="${AETHEROS_HOME:-$HOME/aetheros}"
RUN=1

case "$1" in
  --no-run) RUN=0 ;;
  -h|--help) echo "用法: sh install-termux.sh [--no-run]"; exit 0 ;;
esac

# ---- 下载器：curl 优先，坏/缺则 wget 兜底 ----
if command -v curl >/dev/null 2>&1; then
    GET()  { curl -fsSL "$1"; }
    DLTO() { curl -fL -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
    GET()  { wget -qO- "$1"; }
    DLTO() { wget -O "$2" "$1"; }
else
    echo "!! 需要 curl 或 wget，请先: pkg install curl" >&2
    exit 1
fi

# ---- 探测最新 release 的 tag（不写死版本号）----
# 三级探测，逐级降级（国内网络常见 api/raw 域名被 reset）：
#   1) api.github.com 的 JSON；2) github.com 的 /releases/latest 重定向；
#   3) 都失败 -> FALLBACK 硬编码兜底。
echo "==> 探测最新 release ..."
REL=$(GET "$API" 2>/dev/null \
  | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1 || true)
if [ -z "$REL" ] && command -v curl >/dev/null 2>&1; then
    REL=$(curl -fsSL -o /dev/null -w '%{url_effective}' \
          "https://github.com/$REPO/releases/latest" 2>/dev/null \
          | sed -n 's|.*/releases/tag/||p' | head -n1 || true)
fi
[ -n "$REL" ] || { echo "==> 探测失败，回退 $FALLBACK"; REL="$FALLBACK"; }
BASE_URL="https://github.com/${REPO}/releases/download/${REL}/${ASSET}"
echo "==> 版本 = $REL"

echo "==> AetherOS 手机版一键安装 (${REL})"
echo "    安装目录: ${DEST}"

# ---------- 1. 安装依赖 ----------
if command -v pkg >/dev/null 2>&1; then
  echo "==> [Termux] 安装 qemu-system-aarch64-headless / curl / tar"
  pkg update -y >/dev/null 2>&1
  # 注意: 主仓库里正确的包名是 -headless（装出的二进制仍叫
  # qemu-system-aarch64）; 裸名 qemu-system-aarch64 在 x11-repo 才有。
  pkg install -y qemu-system-aarch64-headless curl tar
  # 可选: Android 宿主 HAL（真机电量/网络）。装上则状态栏显示真实读数，
  # 失败不影响系统运行（自动退化为 sysfs 电量或 bat --）。
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

# ---------- 2. 下载镜像 ----------
mkdir -p "${DEST}"
cd "${DEST}"
echo "==> 下载镜像 (约 20MB):"
echo "    ${BASE_URL}"
DLTO "${BASE_URL}" "${ASSET}"
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
