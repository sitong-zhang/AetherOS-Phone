#!/bin/sh
# 系统环境初始化：注入时区。
# 合成器用 localtime_r（线程安全、不自动 tzset），shell 内 date 读 /etc/TZ；
# 二者都必须通过 TZ 环境变量拿到同一时区，否则锁屏时间与 date 不一致。
#
# 时区口径（可配置，非写死）：
#   1) 内核 cmdline 的 aether.tz=XXX 优先级最高（用于门禁验证「改时区→锁屏时间随之变化」）；
#   2) 否则读 /etc/TZ（系统配置，默认 CST-8）；
#   3) 两者皆无时回落 UTC（正确默认，而非硬写 +8）。
TZ_OVERRIDE=""
for arg in $(cat /proc/cmdline 2>/dev/null); do
    case "$arg" in
        aether.tz=*) TZ_OVERRIDE="${arg#aether.tz=}" ;;
    esac
done
if [ -n "$TZ_OVERRIDE" ]; then
    TZ="$TZ_OVERRIDE"
elif [ -r /etc/TZ ]; then
    TZ="$(cat /etc/TZ)"
else
    TZ="UTC"
fi
export TZ
