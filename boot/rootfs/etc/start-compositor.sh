#!/bin/sh
# 以手机形态启动合成器：竖屏逻辑画布 720x1280，物理帧经内核 video= 覆盖为同分辨率，
# 画布与物理帧 1:1 上屏（文字原分辨率，无 letterbox 缩放）。真实设备提供 /dev/fb0 或
# DRM 时走 present 上屏，未提供时合成器自动降级 headless。
. /etc/aether/env.sh
AETHER_PHONE=1
AETHER_RES=720x1280
# 门禁/自动化：内核 cmdline 带 aether.input_tcp=1 时，开启合成器 debug TCP 输入通道
# （:9230，与物理触摸同一 UI handler）。生产启动不带该 flag，无 TCP、无网络暴露。
for arg in $(cat /proc/cmdline 2>/dev/null); do
    case "$arg" in
        aether.input_tcp=1) AETHER_INPUT_TCP=1 ;;
    esac
done
# 门禁/取证（仅 AETHER_INPUT_TCP=1）：user 模式网络下为合成器 TCP 通道配置 guest 地址，
# 使 QEMU hostfwd（host:9230 -> guest:9230）可路由到合成器。生产启动不设该 flag，无网络动作。
if [ -n "$AETHER_INPUT_TCP" ]; then
    for d in /sys/class/net/eth*; do
        [ -e "$d" ] || continue
        dev=$(basename "$d")
        ip link set "$dev" up 2>/dev/null
        ip addr add 10.0.2.15/24 dev "$dev" 2>/dev/null
    done
fi
export AETHER_PHONE AETHER_RES TZ ${AETHER_INPUT_TCP:+AETHER_INPUT_TCP}
exec /sbin/aether-compositor
