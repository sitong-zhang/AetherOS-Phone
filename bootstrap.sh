#!/bin/bash
# ============================================================
# AetherOS-Phone 环境引导脚本
# 作用：一条命令装齐「交叉编译 + 运行 + 测试」所需工具链与 Git LFS，
#       并把 LFS 指针换成真实二进制，使本仓库开箱即可构建、启动、验证。
# 适用：Debian / Ubuntu（GitHub Actions 的 ubuntu-latest 同族）。
# 用法：
#   bash bootstrap.sh            # 安装工具链 + LFS + 拉取 LFS 二进制
#   bash bootstrap.sh --no-lfs   # 只装工具链（离线 / 不需要大二进制时）
# ============================================================
set -euo pipefail

echo "==> [1/4] 更新 apt 索引"
sudo apt-get update -y

echo "==> [2/4] 安装交叉工具链、QEMU、文件系统与校验工具"
# gcc-aarch64-linux-gnu        : 编译 aether-init / aetherosd / compositor 等 aarch64 静态 ELF
# qemu-system-arm              : 提供 qemu-system-aarch64，跑真内核 + 客机
# e2fsprogs                   : mkfs.ext4，make_rootfs.sh 打包 rootfs
# binutils / file             : readelf / file，CI 断言 ARM64 静态 ELF
# python3                     : 运行 ci/boot-smoke.py 等断言脚本
# busybox                     : 仅本地手动构建 rootfs 时需要，CI 自带交叉编译流程
sudo apt-get install -y \
    gcc-aarch64-linux-gnu \
    qemu-system-arm \
    e2fsprogs \
    binutils \
    file \
    python3 \
    git-lfs

echo "==> [3/4] 初始化 Git LFS（若仓库尚未 init）"
git lfs install --local 2>/dev/null || git lfs install

if [ "${1:-}" != "--no-lfs" ]; then
    echo "==> [4/4] 拉取 LFS 大二进制（内核 / initramfs / .so / APK）"
    git lfs pull || echo "WARN: git lfs pull 失败（可能无网络或仓库无 LFS 对象），可稍后手动执行"
else
    echo "==> [4/4] 跳过 LFS 拉取（--no-lfs）"
fi

echo ""
echo "引导完成。下一步："
echo "  1. 交叉编译用户态并构建 rootfs："
echo "       aarch64-linux-gnu-gcc -static -O2 -o aether-init-aarch64 aether-init.c"
echo "       aarch64-linux-gnu-gcc -static -O2 -o aetherosd-aarch64-static aetherosd.c"
echo "       ./make_rootfs.sh"
echo "  2. 启动客机：  ./boot.sh"
echo "  3. 跑全链路 CI： bash ci/run-ci.sh --skip-core"
echo "详见 README.md 与 AGENTS.md。"
