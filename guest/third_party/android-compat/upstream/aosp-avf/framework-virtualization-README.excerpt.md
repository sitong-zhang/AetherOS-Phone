# Android Virtualization Framework API (AOSP 官方 README 摘录)

来源: https://android.googlesource.com/platform/packages/modules/Virtualization/+/HEAD/libs/framework-virtualization/
获取方式: 官方页面检索摘录（沙箱内 googlesource 直连超时，非 git clone）
获取日期: 2026-11-09
许可: Apache-2.0 (AOSP platform/packages/modules/Virtualization)

> These Java APIs allow an app to configure and run a Virtual Machine running
> Microdroid and execute native code from the app (the payload) within it.
> There is more information on AVF here.
> The API classes are all in the android.system.virtualmachine package.
> All of these APIs were introduced in API level 34 (Android 14).
> The classes may not exist in devices running an earlier version.
> Note that they are all @SystemApi and require the restricted
> android.permission.MANAGE_VIRTUAL_MACHINE permission, so they are not
> available to third party apps. In Android 14 the permission was available
> only to privileged apps; in Android 15 it is available to all preinstalled
> apps. On both versions it can also be granted to other apps via
> adb shell pm grant for development purposes.

## Detecting AVF Support

> The simplest way to detect whether a device has support for AVF is to
> retrieve an instance of the VirtualMachineManager class; if the result is
> not null then the device has support. You can then find out whether
> protected, non-protected VMs, or both are supported using the
> getCapabilities() method. Note that this code requires API level 34 or
> higher:

```java
VirtualMachineManager vmm = context.getSystemService(VirtualMachineManager.class);
if (vmm == null) {
    // AVF is not supported.
} else {
    // AVF is supported.
    int capabilities = vmm.getCapabilities();
    if ((capabilities & CAPABILITY_PROTECTED_VM) != 0) {
        // Protected VMs supported.
    }
    if ((capabilities & CAPABILITY_NON_PROTECTED_VM) != 0) {
        // Non-Protected VMs supported.
    }
}
```

> An alternative for detecting AVF support is to query support for the
> android.software.virtualization_framework system feature. This method will
> work on any API level, and return false if it is below 34:

```java
if (getPackageManager().hasSystemFeature(
        PackageManager.FEATURE_VIRTUALIZATION_FRAMEWORK)) {
    // AVF is supported.
}
```

## Starting a VM

```java
VirtualMachineConfig config = new VirtualMachineConfig.Builder(this)
    .setProtectedVm(true)
    .setPayloadBinaryName("my_payload.so")
    .build();
VirtualMachine vm = vmm.getOrCreate("my vm", config);
vm.setCallback(executor, new VirtualMachineCallback() {...});
vm.run();
```

> The run() method is asynchronous; it returns successfully once the VM is
> starting. Information about the VM, including its configuration, is stored
> in files in your app's private data directory.
