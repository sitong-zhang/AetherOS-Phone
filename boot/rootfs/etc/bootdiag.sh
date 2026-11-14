#!/bin/sh
# bootdiag.sh - A 轨开机诊断
# 从真实内核数据源读取并输出，作为整机链路门禁的可观测断言点。
echo "AETHEROS_BOOT_DIAG: kernel=$(cat /proc/version)"
echo "AETHEROS_BOOT_DIAG: mem=$(grep MemTotal /proc/meminfo | tr -s ' ')"
echo "AETHEROS_BOOT_DIAG: uptime=$(cat /proc/uptime)"
echo "AETHEROS_BOOT_DIAG_FB: fb0=$(test -e /dev/fb0 && echo yes || echo no) dri=$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
echo "AETHEROS_BOOT_DIAG_SERVICES:"
cat /run/aether/services.tsv 2>/dev/null || true
echo "AETHEROS_BOOT_DIAG_DONE=1"
echo 1 > /run/aether/diag-done 2>/dev/null || true
