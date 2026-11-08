#!/bin/sh
# ==============================================================================
#  aetheros-ab-init —— AetherOS A/B 双槽启动选择器（initramfs 内 /init，PID 1）
#
#  这是 AetherOS OTA 的「一次启动选择器」：内核先从这里起步，它按持久分区
#  (/dev/vdc -> /persist) 上的槽位标记决定本次启动哪个系统镜像，然后
#  switch_root 交棒给该槽内的 AetherOS 原生 init (/sbin/init)。
#
#  真实行为（与 aether-ota.c 的槽位契约一一对应）：
#    1) 挂载 proc/sys/devtmpfs；devtmpfs 提供 /dev/vd{a,b,c}
#    2) rw 挂载 persist 分区到 /persist；失败则保住现场（落 shell），
#       绝不静默继续——静默回退 A 槽会掩盖持久分区损坏
#    3) 读 /persist/slot（A|B；缺失或乱码按 A，与 aether-ota 缺省一致）
#    4) 若 /persist/recovery 存在 -> 恢复模式：
#         rw 挂载活跃槽到 /mnt/slot（便于现场 chroot 修复），
#         执行 aether-ota --apply（校验活跃槽 + 清除恢复标志），
#         打印 AETHEROS_RECOVERY_MODE 取证 banner，随后 reboot -f
#    5) 否则正常启动：
#         rw 挂载所选槽 (/dev/vda|/dev/vdb) 到 /newroot，
#         校验 /sbin/init 可执行，exec switch_root 交棒
#
#  槽位翻转为什么不会变砖：aether-ota --switch 在提交翻转前先挂载目标槽
#  校验 /sbin/init（见 aether-ota.c validate_slot），本选择器看到的活跃槽
#  永远是已被校验过的。若活跃槽仍损坏（如人为破坏），本选择器打印 FATAL
#  并落 shell，而不是伪造一个成功启动。
# ==============================================================================

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH

mount -t proc     none  /proc
mount -t sysfs    none  /sys
mount -t devtmpfs none  /dev

mkdir -p /newroot /persist /mnt/slot /mnt/otacheck /proc /sys /dev

echo "aetheros-ab: initramfs selector up (pid=$$)"

# ---- 持久分区 ---------------------------------------------------------------
if ! mount -t ext4 -o rw /dev/vdc /persist; then
    echo "aetheros-ab: FATAL cannot mount persist partition (/dev/vdc)"
    echo "aetheros-ab: dropping to emergency shell (no silent fallback)"
    exec sh
fi

SLOT=A
if [ -r /persist/slot ]; then
    _s=$(cat /persist/slot 2>/dev/null)
    case "$_s" in
        *B*) SLOT=B ;;
        *)   SLOT=A ;;
    esac
fi
echo "aetheros-ab: persist mounted, slot marker=$SLOT"

# ---- 恢复模式 ----------------------------------------------------------------
if [ -e /persist/recovery ]; then
    echo "AETHEROS_RECOVERY_MODE: active_slot=$SLOT"
    # rw 挂载活跃槽到 /mnt/slot：恢复环境可现场修复（chroot /mnt/slot ...）
    if [ "$SLOT" = B ]; then
        mount -t ext4 -o rw /dev/vdb /mnt/slot \
            || echo "aetheros-ab: recovery: slot B mount failed (apply will validate)"
    else
        mount -t ext4 -o rw /dev/vda /mnt/slot \
            || echo "aetheros-ab: recovery: slot A mount failed (apply will validate)"
    fi
    echo "aetheros-ab: recovery: running aether-ota --apply (validate + clear flag)"
    /aether-ota --apply
    echo "aetheros-ab: recovery: apply done, rebooting to slot $SLOT"
    sync
    reboot -f
    echo "aetheros-ab: FATAL reboot -f returned; dropping to shell"
    exec sh
fi

# ---- 正常启动：挂载所选槽并交棒 ---------------------------------------------
DEV=/dev/vda
[ "$SLOT" = B ] && DEV=/dev/vdb

echo "aetheros-ab: booting slot $SLOT ($DEV)"
if ! mount -t ext4 -o rw "$DEV" /newroot; then
    echo "aetheros-ab: FATAL slot $SLOT ($DEV) failed to mount"
    echo "aetheros-ab: dropping to emergency shell (no silent fallback)"
    exec sh
fi
if [ ! -x /newroot/sbin/init ]; then
    echo "aetheros-ab: FATAL slot $SLOT has no executable /sbin/init"
    umount /newroot
    exec sh
fi

echo "aetheros-ab: switch_root $DEV -> /sbin/init"
exec switch_root /newroot /sbin/init

echo "aetheros-ab: FATAL switch_root returned; dropping to shell"
exec sh
