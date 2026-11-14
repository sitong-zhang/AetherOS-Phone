#!/bin/sh
# 以手机形态启动合成器：竖屏逻辑画布，letterbox 缩放到物理帧缓冲。
# 真实设备提供 /dev/fb0 或 DRM 时走 present 上屏，未提供时合成器自动降级 headless。
AETHER_PHONE=1
AETHER_RES=720x1280
export AETHER_PHONE AETHER_RES
exec /sbin/aether-compositor
