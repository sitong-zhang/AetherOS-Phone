#!/data/data/com.termux/files/usr/bin/sh
# aether-android-bridge.sh — Termux 侧「Android 宿主」桥接
#
# 作用：把 Android 真机的电量 / 充电 / Wi-Fi / 通知数写成一份 JSON 快照，
# 放进 QEMU 9p 共享目录（hostshare）。AetherOS 客户机里的 aether-hal-android
# 服务读取该快照，SystemUI 状态栏据此显示真实电量——这就是「安卓兼容层」
# 的 HAL 后端：AetherOS Core 只看到硬件事实，不知道它来自 Android。
#
# 用法：
#   sh aether-android-bridge.sh [共享目录]      # 常驻，每 3 秒刷新
#   sh aether-android-bridge.sh [共享目录] --once   # 只写一次（自检用）
#
# 数据来源优先级（逐项独立回退，缺什么就诚实留空）：
#   电量：termux-battery-status  -> /sys/class/power_supply/battery
#   Wi-Fi：termux-wifi-connectioninfo -> 留空
#   通知：termux-notification-list -> 留空
#
# 依赖：termux-api 包 + Termux:API 应用（可选）。没有也能跑，只是数据显示少。

SHARE="${1:-$(pwd)/share}"
ONCE=0
[ "${2:-}" = "--once" ] && ONCE=1
OUT="$SHARE/android-host.json"

mkdir -p "$SHARE" 2>/dev/null

log() { echo "[bridge] $*" >&2; }

have() { command -v "$1" >/dev/null 2>&1; }

# 去除 JSON 字符串里会破坏我们输出结构的字符
sanitize() { printf '%s' "$1" | tr -d '"\\\n\r\t'; }

read_battery() {
    BAT_PCT=-1
    BAT_CHG=0
    if have termux-battery-status; then
        j=$(termux-battery-status 2>/dev/null | tr -d '\n')
        [ -n "$j" ] || return
        p=$(printf '%s' "$j" | sed -n 's/.*"percentage":[ ]*\([0-9]\{1,3\}\).*/\1/p')
        st=$(printf '%s' "$j" | sed -n 's/.*"status":[ ]*"\([A-Za-z_]*\)".*/\1/p')
        [ -n "$p" ] && BAT_PCT=$p
        case "$st" in
            CHARGING|FULL) BAT_CHG=1 ;;
        esac
        return
    fi
    # sysfs 回退（部分设备路径不同，逐个试）
    for c in /sys/class/power_supply/battery/capacity \
             /sys/class/power_supply/bms/capacity; do
        if [ -r "$c" ]; then
            BAT_PCT=$(cat "$c" 2>/dev/null)
            break
        fi
    done
    for s in /sys/class/power_supply/battery/status \
             /sys/class/power_supply/bms/status; do
        if [ -r "$s" ]; then
            case "$(cat "$s" 2>/dev/null)" in
                Charging|Full) BAT_CHG=1 ;;
            esac
            break
        fi
    done
    [ -n "$BAT_PCT" ] && [ "$BAT_PCT" -ge 0 ] 2>/dev/null || BAT_PCT=-1
}

read_wifi() {
    WIFI=""
    have termux-wifi-connectioninfo || return
    j=$(termux-wifi-connectioninfo 2>/dev/null | tr -d '\n')
    [ -n "$j" ] || return
    s=$(printf '%s' "$j" | sed -n 's/.*"ssid":[ ]*"\([^"]*\)".*/\1/p')
    WIFI=$(sanitize "$s")
}

read_notif() {
    NOTIF=0
    have termux-notification-list || return
    n=$(termux-notification-list 2>/dev/null | grep -o '"id"' | wc -l | tr -d ' ')
    [ -n "$n" ] && NOTIF=$n
}

emit() {
    read_battery
    read_wifi
    read_notif
    printf '{"battery":%s,"charging":%s,"wifi":"%s","cellular":"","notifications":%s}\n' \
        "$BAT_PCT" "$BAT_CHG" "$WIFI" "$NOTIF" > "$OUT.tmp" 2>/dev/null \
        && mv -f "$OUT.tmp" "$OUT" 2>/dev/null
}

if [ "$ONCE" = "1" ]; then
    emit
    log "已写入 $OUT: $(cat "$OUT" 2>/dev/null)"
    exit 0
fi

log "常驻，每 3 秒写 $OUT（Ctrl-C 退出）"
while :; do
    emit
    sleep 3
done
