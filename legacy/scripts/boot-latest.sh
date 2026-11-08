#!/bin/sh
# boot-latest.sh —— 一条命令「装 + 跑」最新版 AetherOS。
#
# 永远拉 GitHub 最新 release（releases/latest/download，不写死版本号：
# phone-v7/v8 出来照跑）。下载通道走「直连 + 国内可达镜像」自动兜底，
# 哪个通就用哪个——Termux 里直连 github.com 被墙(超时/reset)也能装。
#
# 用法（Termux，或任何带 sh + curl/wget + tar + qemu-system-aarch64 的 Linux）：
#   curl -fsSL "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh" | sh
#   # 备用镜像（上面那条不通时换这条）：
#   curl -fsSL "https://ghfast.top/https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh" | sh
#   # 网络通畅时也可用直连（最原始那条）：
#   curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/boot-latest.sh | sh
#
# 可选环境变量：
#   AETHER_MEM=384M   低内存手机（3GB 以下）调小内存
#   AETHER_SMP=1      单 vCPU，TCG 软件模拟下更稳
#   AETHER_HOME=DIR   安装/运行目录（默认 ~/aetheros）
#   AETHER_LOGS=1     前台滚动内核串口日志（默认后台，不占屏）
set -eu

REPO="sitong-zhang/AetherOS-Phone"
PKG="aetheros-phone.tar.gz"
FALLBACK="phone-v6"   # /latest/download 全失败时，兜底到这个版本的固定路径
WORK="${AETHER_HOME:-$HOME/aetheros}"

# ---- 下载器：curl 优先，坏/缺则 wget 兜底 ----
# Termux 里 curl 可能因 libssl 不匹配而 CANNOT LINK——真正跑 --version 才知道能不能用。
if command -v curl >/dev/null 2>&1 && curl --version >/dev/null 2>&1; then
    FETCHER="curl"
elif command -v wget >/dev/null 2>&1; then
    FETCHER="wget"
else
    echo "[aether] 需要 curl 或 wget，请先: pkg install curl" >&2
    exit 1
fi
echo "[aether] 下载器: $FETCHER"

# ---- 镜像通道：依次尝试，哪个能下就用哪个 ----
# 每个元素是拼在 https://github.com 之前的「前缀」。反代用法:
#   https://ghfast.top/https://github.com/sitong-zhang/AetherOS-Phone/...
# （整个 github URL 作为反代的路径，实测可正常透传 release 资产）
MIRRORS="
https://github.com
https://gh-proxy.com/https://github.com
https://ghfast.top/https://github.com
"

# fetch_rel <repo内相对路径> [输出文件]
#   依次套用各镜像前缀尝试下载；输出文件为空则把内容打到 stdout。
#   全部失败返回 1（不触发 set -e，由调用方判断）。
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
        echo "[aether] 通道不可用: ${_base}" >&2
    done
    return 1
}

# ---- 探测最新 release tag（仅用于友好显示，失败不影响安装）----
probe_latest() {
    _tag=""
    _tag=$(curl -fsSL --max-time 20 "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
        | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1) || true
    if [ -z "$_tag" ]; then
        _tag=$(curl -fsSL --max-time 20 "https://ghfast.top/https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
            | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1) || true
    fi
    if [ -z "$_tag" ]; then
        _tag=$(curl -fsSL -o /dev/null -w '%{url_effective}' --max-time 20 "https://github.com/$REPO/releases/latest" 2>/dev/null \
            | sed -n 's|.*/releases/tag/||p' | head -n1) || true
    fi
    echo "$_tag"
}

mkdir -p "$WORK"
cd "$WORK"
echo "[aether] 工作目录: $WORK"

LATEST=$(probe_latest)
echo "[aether] 最新版 = ${LATEST:-（latest）}"

# ---- 下载镜像包（永远最新版；失败兜底到固定版本路径）----
echo "[aether] 下载 $PKG ..."
if ! fetch_rel "releases/latest/download/$PKG" "$WORK/$PKG"; then
    echo "[aether] /latest/download 失败，兜底到 $FALLBACK ..."
    if ! fetch_rel "releases/download/$FALLBACK/$PKG" "$WORK/$PKG"; then
        echo "[aether] 所有通道都失败。" >&2
        echo "[aether] 1) 试设代理: export ALL_PROXY=socks5://127.0.0.1:1080 或 http://127.0.0.1:7890" >&2
        echo "[aether] 2) 换镜像入口重跑本脚本（见脚本顶部注释）" >&2
        echo "[aether] 3) 手动下载后放 $WORK/$PKG 再重跑" >&2
        exit 1
    fi
fi
echo "[aether] 下载完成: $(du -h "$WORK/$PKG" | cut -f1)"

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
        echo "[aether] 自动安装 QEMU 失败，请手动: pkg install qemu-system-aarch64-headless" >&2
        echo "[aether] 装好后重新跑: sh $WORK/phone-boot.sh" >&2
        exit 1
    fi
fi

# 版本号显示：解包里若有 version 文件则读取
if [ -f "$WORK/version" ]; then
    VER=$(cat "$WORK/version" 2>/dev/null || true)
else
    VER="${LATEST:-latest}"
fi

echo "[aether] 启动 AetherOS $VER（TCG 软件模拟，首次约 1-3 分钟）"
echo "[aether] 画面: VNC 查看器(推荐 AVNC) 连 127.0.0.1:5900 | 退出: Ctrl-A 再按 X"
echo "[aether] 交互: 先【点按屏幕任意处解锁】-> 再点按启动器应用行(或数字键 1-9)启动应用"

# 默认后台跑：Termux 不留半屏内核日志，AVNC 全屏就是一整部手机。
if [ "${AETHER_LOGS:-0}" = "1" ]; then
    exec sh "$WORK/phone-boot.sh"
fi

LOG="$WORK/aetheros-boot.log"
echo "[aether] 后台启动中（日志: $LOG）..."
nohup sh "$WORK/phone-boot.sh" > "$LOG" 2>&1 &
sleep 8
if ps -A 2>/dev/null | grep -q "[q]emu-system-aarch64"; then
    echo "[aether] 已启动。现在打开 AVNC 连 127.0.0.1:5900，把画面设为『填满屏幕』"
    echo "[aether] 你会先看到锁屏（大时钟 + 日期 + 锁图标）——点按屏幕任意处解锁"
    echo "[aether] 停止: pkill qemu-system-aarch64 | 日志: tail -f $LOG"
else
    echo "[aether] 启动失败，日志尾部 20 行："
    tail -20 "$LOG" 2>/dev/null
    exit 1
fi
