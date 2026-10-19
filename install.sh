#!/bin/sh
# install.sh — 兼容别名，转发到 install-termux.sh
# 用法: sh install.sh [--no-run]
DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || DIR="."
if [ -f "$DIR/install-termux.sh" ]; then
  exec sh "$DIR/install-termux.sh" "$@"
fi
# 远程回退：直接拉取主分支脚本
exec sh -c 'curl -fsSL https://raw.githubusercontent.com/sitong-zhang/AetherOS-Phone/main/install-termux.sh | sh "$@"' _ "$@"
