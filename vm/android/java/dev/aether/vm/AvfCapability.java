package dev.aether.vm;

import android.content.Context;
import android.content.pm.PackageManager;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;

/**
 * AVF / pKVM 能力探测（只读，不修改设备状态）。
 *
 * 检测口径逐条照搬 AOSP packages/modules/Virtualization 官方 README 与
 * "Try AVF" 调试文档，见
 * guest/third_party/android-compat/upstream/aosp-avf/pkvm-detection.excerpt.md。
 *
 * 分级：
 *   0 NONE              设备不支持 AVF
 *   1 FRAMEWORK         声明 android.software.virtualization_framework feature
 *   2 NON_PROTECTED     支持非受保护 VM
 *   3 PROTECTED         pKVM 支持受保护 VM
 *   4 APP_USABLE        本应用具备 MANAGE_VIRTUAL_MACHINE 且 VirtualMachineManager 可用
 *
 * 【不得曲解的平台事实】VirtualMachineManager / VirtualMachineConfig 均为
 * @SystemApi，需受限权限 android.permission.MANAGE_VIRTUAL_MACHINE；Android 14
 * 仅特权应用、Android 15 仅预装应用可获得，普通安装的第三方 APK 不可用。且 AVF
 * 面向 headless / Microdroid 隔离负载，不提供图形 HAL，无法承载带桌面的完整客机
 * 操作系统。因此即便探测到 pKVM，本应用仍以 QEMU TCG 启动 AetherOS；探测结果
 * 只作为前置信号写入内核命令行，供客机 aether-android 服务读取并决定兼容层提示。
 */
public final class AvfCapability {

    public static final int NONE = 0;
    public static final int FRAMEWORK = 1;
    public static final int NON_PROTECTED = 2;
    public static final int PROTECTED = 3;
    public static final int APP_USABLE = 4;

    /** AOSP "Try AVF"：任意 API 级可用的 feature 名（API 34 起有常量，这里用字面量兼容 minSdk 21）。 */
    private static final String FEATURE_VIRTUALIZATION_FRAMEWORK =
            "android.software.virtualization_framework";
    private static final String VMM_CLASS =
            "android.system.virtualmachine.VirtualMachineManager";
    private static final String PERMISSION_MANAGE_VM =
            "android.permission.MANAGE_VIRTUAL_MACHINE";

    private AvfCapability() {}

    /** 只读系统属性：优先反射 SystemProperties，失败则退回 getprop 进程。 */
    private static String getProperty(String key) {
        String v = null;
        try {
            Class<?> c = Class.forName("android.os.SystemProperties");
            v = (String) c.getMethod("get", String.class).invoke(null, key);
        } catch (Throwable ignored) {
            // 隐藏 API 在部分 ROM 上被限制，退回命令行
        }
        if (v != null && v.length() > 0) return v;
        BufferedReader r = null;
        try {
            Process p = Runtime.getRuntime().exec(new String[] {"/system/bin/getprop", key});
            r = new BufferedReader(new InputStreamReader(p.getInputStream()), 256);
            String line = r.readLine();
            return line == null ? "" : line.trim();
        } catch (Throwable t) {
            return "";
        } finally {
            if (r != null) try { r.close(); } catch (Throwable ignored) {}
        }
    }

    private static boolean exists(String path) {
        try { return new File(path).exists(); } catch (Throwable t) { return false; }
    }

    private static boolean isTrue(String s) {
        return "true".equalsIgnoreCase(s) || "1".equals(s);
    }

    /**
     * 返回 0..4 的能力级别。只读，不启动任何 VM，不申请任何权限。
     * 逐项对应 AOSP 检测清单：feature / boot 属性 / APEX / KVM 节点 / VMM 服务 / 权限。
     */
    public static int probe(Context ctx) {
        int level = NONE;
        try {
            PackageManager pm = ctx.getPackageManager();
            if (pm != null && pm.hasSystemFeature(FEATURE_VIRTUALIZATION_FRAMEWORK)) {
                level = FRAMEWORK;
            }
        } catch (Throwable t) {
            Log.w(AetherApp.TAG, "AVF feature 查询失败", t);
        }

        boolean vmSupported = isTrue(getProperty("ro.boot.hypervisor.vm.supported"));
        boolean protectedSupported =
                isTrue(getProperty("ro.boot.hypervisor.protected_vm.supported"));
        boolean apexVm = exists("/apex/com.android.virt/bin/vm");
        boolean kvm = exists("/dev/kvm");

        if (level >= FRAMEWORK && (vmSupported || apexVm || kvm)) {
            level = NON_PROTECTED;
        }
        if (level >= NON_PROTECTED && protectedSupported && kvm) {
            level = PROTECTED;
        }

        // 只有 API 34+、VMM 服务存在、且本应用被授予受限权限时才升到 APP_USABLE。
        if (level >= PROTECTED && android.os.Build.VERSION.SDK_INT >= 34) {
            try {
                Class.forName(VMM_CLASS);
                boolean granted = ctx.checkCallingOrSelfPermission(PERMISSION_MANAGE_VM)
                        == PackageManager.PERMISSION_GRANTED;
                Object vmm = ctx.getSystemService(VMM_CLASS);
                if (granted && vmm != null) level = APP_USABLE;
            } catch (Throwable t) {
                Log.i(AetherApp.TAG, "VirtualMachineManager 不可用（第三方 APK 常见）");
            }
        }
        return level;
    }

    /** 人可读的能力名称，写入诊断浮层与内核参数。 */
    public static String name(int level) {
        switch (level) {
            case APP_USABLE:     return "app-usable";
            case PROTECTED:      return "protected";
            case NON_PROTECTED:  return "non-protected";
            case FRAMEWORK:      return "framework";
            default:             return "none";
        }
    }

    /**
     * 客机内核命令行参数。AetherApp 把它写进 vm.conf.final 的 avf_args，
     * 由 core/vm_config.c 追加到 -append，客机 aether-android 服务读 /proc/cmdline。
     */
    public static String kernelArgs(Context ctx) {
        int level = probe(ctx);
        // pKVM 可用（>= PROTECTED）是安卓兼容层可启用硬件加速的前提；
        // 第三方 APK 是否拿到 SystemApi 权限单独标记，不冒充已切换 AVF。
        int pkvm = level >= PROTECTED ? 1 : 0;
        int appApi = level >= APP_USABLE ? 1 : 0;
        return "aether.avf.level=" + level
                + " aether.avf.name=" + name(level)
                + " aether.avf.pkvm=" + pkvm
                + " aether.avf.appapi=" + appApi;
    }
}
