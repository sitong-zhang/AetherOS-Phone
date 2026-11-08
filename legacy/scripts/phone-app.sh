#!/bin/sh
# phone-app.sh —— 一条命令启动「AetherOS 手机 App」。
#
# 打开先是一个终端界面 -> 点运行自动安装最新系统 -> 系统直接在这个
# App 里显示（内嵌 VNC 显示器，可触摸点按解锁、启动应用），彻底告别
# 「Termux 看串口 + 单独开 VNC 查看器」的割裂流程。
#
# 用法（Termux）：
#   curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh" | sh
#   # 备用镜像（上面那条不通时换这条）：
#   curl -fsSL "https://ghfast.top/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh" | sh
#   # 直连（网络通畅时）：
#   curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/phone-app.sh | sh
#
# 可选环境变量：
#   AETHER_HOME=DIR    安装/运行目录（默认 ~/aetheros-app）
#   AETHER_PORT=8080   App 服务端口（默认 8080）
#   AETHER_MEM=512M    虚拟机内存
#   AETHER_SMP=2       虚拟机 vCPU 数
set -eu

REPO="sitong-zhang/AetherOS-Phone"
APP_PKG="aether-phone-app.tar.gz"
FALLBACK="phone-v6"   # /latest/download 全失败时兜底到这个版本的固定路径
HOME_DIR="${AETHER_HOME:-$HOME/aetheros-app}"
PORT="${AETHER_PORT:-8080}"

# ---- 下载器：curl 优先，坏/缺则 wget 兜底 ----
if command -v curl >/dev/null 2>&1 && curl --version >/dev/null 2>&1; then
    FETCHER="curl"
elif command -v wget >/dev/null 2>&1; then
    FETCHER="wget"
else
    echo "[aether] 需要 curl 或 wget，请先: pkg install curl" >&2
    exit 1
fi
echo "[aether] 下载器: $FETCHER"

# ---- 镜像通道 ----
MIRRORS="
https://github.com
https://gh-proxy.com/https://github.com
https://ghfast.top/https://github.com
"

fetch_rel() {
    _rel="$1"; _out="$2"
    for _base in $MIRRORS; do
        _url="${_base}/${REPO}/${_rel}"
        if [ "$FETCHER" = curl ]; then
            if curl -fL --retry 3 --max-time 180 -o "$_out" "$_url" 2>/dev/null; then return 0; fi
        else
            if wget -O "$_out" --timeout=180 "$_url" 2>/dev/null; then return 0; fi
        fi
        echo "[aether] 通道不可用: ${_base}" >&2
    done
    return 1
}

mkdir -p "$HOME_DIR"
cd "$HOME_DIR"

# ---- 下载 App 包（server.py + 前端；永远最新版）----
if [ -f "$HOME_DIR/app/server.py" ] && [ "${1:-}" != "--reinstall" ]; then
    echo "[aether] App 已存在，跳过下载（如需重装: sh $HOME_DIR/phone-app.sh --reinstall）"
else
    echo "[aether] 下载 App 包 $APP_PKG ..."
    if ! fetch_rel "releases/latest/download/$APP_PKG" "$HOME_DIR/$APP_PKG"; then
        echo "[aether] /latest/download 失败，兜底到 $FALLBACK ..."
        if ! fetch_rel "releases/download/$FALLBACK/$APP_PKG" "$HOME_DIR/$APP_PKG"; then
            echo "[aether] 所有通道都失败。" >&2
            echo "[aether] 1) 换镜像入口重跑（见脚本顶部注释）" >&2
            echo "[aether] 2) 或设代理: export ALL_PROXY=socks5://127.0.0.1:1080" >&2
            echo "[aether] 3) 或手动下载后放 $HOME_DIR/$APP_PKG 再重跑" >&2
            exit 1
        fi
    fi
    echo "[aether] 解包 App ..."
    tar xzf "$HOME_DIR/$APP_PKG" -C "$HOME_DIR"
    rm -f "$HOME_DIR/$APP_PKG"
fi

# ---- 依赖检查：python3 必需，qemu 缺失则自动安装 ----
if ! command -v python3 >/dev/null 2>&1; then
    echo "[aether] 需要 python3，Termux 请执行: pkg install python" >&2
    exit 1
fi

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
        echo "[aether] 自动安装 QEMU 失败，请手动: pkg install qemu-system-aarch64-headless" >&2
        echo "[aether] 装好后重新跑本命令即可" >&2
        exit 1
    fi
fi

# ---- 端口占用检查：顺延到第一个空闲端口（8080 起，最多试到 8099）----
if command -v ss >/dev/null 2>&1; then
    _tries=0
    while ss -ltn 2>/dev/null | grep -q ":$PORT " && [ "$_tries" -lt 100 ]; do
        if [ "$PORT" -eq 8080 ]; then echo "[aether] 端口 8080 被占用，顺延中 ..."; fi
        PORT=$((PORT+1)); _tries=$((_tries+1))
    done
    [ "$PORT" -ne 8080 ] && echo "[aether] 最终选用端口: $PORT"
fi

# ---- 后台启动服务 ----
export AETHER_HTTP_PORT="$PORT"
export AETHER_APP_DIR="$HOME_DIR"
echo "[aether] 启动 App 服务 (端口 $PORT) ..."
if [ -f "$HOME_DIR/server.log" ]; then : > "$HOME_DIR/server.log"; fi
# 后台运行；Termux 下关掉终端也继续跑
setsid python3 "$HOME_DIR/app/server.py" >> "$HOME_DIR/server.log" 2>&1 &
sleep 2

# 等待端口就绪（最多 10 秒）
i=0
while [ $i -lt 10 ]; do
    if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":$PORT "; then break; fi
    i=$((i+1)); sleep 1
done

echo ""
echo "[aether] ================================================"
echo "[aether]  AetherOS App 已就绪: http://127.0.0.1:$PORT"
echo "[aether]  打开后先看到【终端界面】-> 点「运行」自动装系统"
echo "[aether]  装完自动切到【系统画面】，点按屏幕即可解锁使用"
if command -v termux-open-url >/dev/null 2>&1; then
    termux-open-url "http://127.0.0.1:$PORT"
    echo "[aether]  已在浏览器打开（可「添加到主屏幕」当独立 App）"
else
    echo "[aether]  请在浏览器打开: http://127.0.0.1:$PORT"
fi
echo "[aether] 日志: tail -f $HOME_DIR/server.log"
echo "[aether] 停止: kill \$(ss -ltnp | grep ':$PORT ' | grep -oP 'pid=\K[0-9]')"
echo "[aether] ================================================"
