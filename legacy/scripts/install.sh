#!/bin/sh
# install.sh — 兼容别名，转发到 install-termux.sh
# 用法: sh install.sh [--no-run]
DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || DIR="."
if [ -f "$DIR/install-termux.sh" ]; then
  exec sh "$DIR/install-termux.sh" "$@"
fi
# 远程回退：拉取最新 release 的安装脚本（用 github.com 域名，
# raw.githubusercontent.com 在国内网络常被连接重置）
exec sh -c 'curl -fsSL https://github.com/sitong-zhang/AetherOS-Phone/releases/latest/download/install-termux.sh | sh "$@"' _ "$@"
