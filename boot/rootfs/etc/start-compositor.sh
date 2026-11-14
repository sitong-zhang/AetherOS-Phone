#!/bin/sh
# 以手机形态启动合成器：竖屏逻辑画布 720x1280，物理帧经内核 video= 覆盖为同分辨率，
# 画布与物理帧 1:1 上屏（文字原分辨率，无 letterbox 缩放）。真实设备提供 /dev/fb0 或
# DRM 时走 present 上屏，未提供时合成器自动降级 headless。
. /etc/aether/env.sh
AETHER_PHONE=1
AETHER_RES=720x1280
export AETHER_PHONE AETHER_RES TZ
exec /sbin/aether-compositor
