#!/bin/sh
# 系统环境初始化：注入时区。
# 合成器用 localtime_r（线程安全、不自动 tzset），shell 内 date 读 /etc/TZ；
# 二者都必须通过 TZ 环境变量拿到同一时区，否则锁屏时间与 date 不一致。
# 默认 UTC+8（CST-8），设置应用可改写 /etc/TZ 后重启生效。
if [ -r /etc/TZ ]; then
    TZ="$(cat /etc/TZ)"
else
    TZ="CST-8"
fi
export TZ
