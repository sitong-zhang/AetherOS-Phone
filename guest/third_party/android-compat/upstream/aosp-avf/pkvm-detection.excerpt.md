# pKVM / AVF 能力检测口径（AOSP 官方与社区实践汇总）

来源:
  - https://source.android.com/docs/core/virtualization (AVF)
  - https://android.googlesource.com/platform/packages/modules/Virtualization/ (README, debugging)
  - https://android.stackexchange.com/questions/258842/ (getprop / bootconfig 实测)
获取日期: 2026-11-09

只读检测项（不修改设备状态）：
  getprop ro.boot.hypervisor.protected_vm.supported   -> "true" 表示支持受保护 VM
  getprop ro.boot.hypervisor.vm.supported             -> "true" 表示支持非受保护 VM
  grep hypervisor /proc/bootconfig                    -> androidboot.hypervisor.* 参数
  ls /apex/com.android.virt/bin/vm                   -> AVF APEX 是否存在
  ls -la /dev/kvm                                    -> KVM 设备节点
  getprop ro.product.cpu.abi                         -> 受保护 VM 需要 arm64
  getprop | grep -i hypervisor                       -> 属性总览

支持设备范围（AOSP getting started）：Pixel 6 及之后的 Google 参考机 +
Cuttlefish(aosp_cf_x86_64_phone)；Cuttlefish 只支持非受保护 VM。
部分早期参考机需 Android 14 + 解锁 bootloader 后 fastboot oem pkvm enable。

关键限制（决定本项目实现形态）：
  1. VirtualMachineManager / VirtualMachineConfig 均为 @SystemApi，需受限权限
     android.permission.MANAGE_VIRTUAL_MACHINE，第三方 APK 不可用。
  2. AVF 面向 headless / Microdroid 隔离负载，不提供图形 HAL、无 UI，
     无法承载「带桌面的完整客机操作系统」。
  3. 因此普通安装的第三方 APK 无法用 AVF 启动 AetherOS 内核 + system.qcow2。
