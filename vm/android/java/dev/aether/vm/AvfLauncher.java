package dev.aether.vm;

import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.lang.reflect.Method;

/**
 * 虚拟机启动路径决策：AVF / pKVM 优先探测，不可用则 fallback 现有 QEMU TCG。
 *
 * 【技术边界，必须如实说明，不得对外宣称 AVF 路径已跑通】
 * AOSP 的 VirtualMachine API 用反射调用（minSdk 21 不能编译期依赖 API 34 的
 * @SystemApi）。但对一个「普通安装的第三方 APK」而言，AVF 替代 QEMU TCG 存在
 * 三条硬约束，任一不成立即必须保持 QEMU TCG：
 *   1. 权限：MANAGE_VIRTUAL_MACHINE 为受限权限，Android 14 仅特权应用、
 *      Android 15 仅预装应用；普通安装 APK 无法获得（AvfCapability.probe < APP_USABLE）。
 *   2. 图形：AVF 面向 headless / Microdroid 隔离负载，不提供图形 HAL，
 *      无法承载「带桌面的完整客机操作系统」。AetherOS 的桌面合成器依赖
 *      /dev/fb0 + 输入设备直通，这在受保护 VM 内不成立。
 *   3. 载荷：AVF 的 VirtualMachineConfig 需要签名的 payload binary 与
 *      instance.img；本工程交付的是 kernel.img + system.qcow2 完整 Linux 盘，
 *      不是 Microdroid payload，不能直接作为 pVM 载荷加载。
 *
 * 因此 decide() 的默认且实际生效路径恒为 QEMU TCG；AVF 分支保留为
 * capability-gated 的反射入口，供系统签名 / 预装 / 具备 pKVM 图形扩展的场景
 * 演进，调用失败一律降级并在诊断信息里写明原因。探测结果通过内核命令行
 * aether.avf.* 透传给客机，供 aether-android 兼容层给出「需要更高版本系统」提示。
 */
public final class AvfLauncher {

    /** 实际采用的启动路径。 */
    public static final int MODE_QEMU_TCG = 0;   /* 现有链路：SDL2 + QEMU 8.2.0 aarch64 TCG */
    public static final int MODE_AVF = 1;        /* 预留：AVF VirtualMachine API（未启用） */

    private static final String VMM_CLASS =
            "android.system.virtualmachine.VirtualMachineManager";
    private static final String CONFIG_BUILDER =
            "android.system.virtualmachine.VirtualMachineConfig$Builder";

    private AvfLauncher() {}

    /** 决策结果：mode + 人类可读原因。 */
    public static final class Decision {
        public final int mode;
        public final int capability;
        public final String reason;
        Decision(int mode, int capability, String reason) {
            this.mode = mode;
            this.capability = capability;
            this.reason = reason;
        }
        public String label() {
            return mode == MODE_AVF ? "AVF / pKVM" : "QEMU TCG";
        }
    }

    /**
     * 决定本次开机走哪条路径。只读探测，不启动 VM，不改变现有 QEMU 启动链。
     * forceAvf 仅供开发/系统签名场景显式开启（默认 false）。
     */
    public static Decision decide(Context ctx, boolean forceAvf) {
        int cap = AvfCapability.probe(ctx);
        if (cap < AvfCapability.PROTECTED) {
            return new Decision(MODE_QEMU_TCG, cap,
                    "设备未提供 pKVM 受保护虚拟机能力，使用 QEMU TCG");
        }
        if (Build.VERSION.SDK_INT < 34) {
            return new Decision(MODE_QEMU_TCG, cap,
                    "系统低于 Android 14，无 VirtualMachine API，使用 QEMU TCG");
        }
        if (cap < AvfCapability.APP_USABLE) {
            return new Decision(MODE_QEMU_TCG, cap,
                    "检测到 pKVM，但本应用无 MANAGE_VIRTUAL_MACHINE 受限权限；"
                            + "AVF 亦不提供图形 HAL，无法承载桌面客机，使用 QEMU TCG");
        }
        if (!forceAvf) {
            return new Decision(MODE_QEMU_TCG, cap,
                    "pKVM 与权限齐备，但本工程载荷为完整 Linux 内核+系统盘，"
                            + "非 Microdroid payload，保持已验证的 QEMU TCG 启动链");
        }
        // capability-gated 反射入口：只有显式 force 且具备权限时才尝试。
        if (tryStartAvf(ctx)) {
            return new Decision(MODE_AVF, cap, "AVF 受保护虚拟机已请求启动（实验路径）");
        }
        return new Decision(MODE_QEMU_TCG, cap,
                "AVF 反射调用失败或无 payload，降级 QEMU TCG");
    }

    /**
     * 按 AOSP 官方 README 的 VirtualMachineConfig.Builder / getOrCreate / run
     * 顺序做反射调用。任何一步失败即返回 false（不改设备状态、不吞异常）。
     * 该方法不申请权限、不下载镜像、不伪造成功。
     */
    private static boolean tryStartAvf(Context ctx) {
        try {
            Class<?> vmmCls = Class.forName(VMM_CLASS);
            Class<?> builderCls = Class.forName(CONFIG_BUILDER);
            Object vmm = ctx.getSystemService(VMM_CLASS);
            if (vmm == null) {
                Log.i(AetherApp.TAG, "AVF: VirtualMachineManager 为空，降级");
                return false;
            }
            // new VirtualMachineConfig.Builder(context).setProtectedVm(true)
            Object builder = builderCls.getConstructor(Context.class).newInstance(ctx);
            Method setProtected = builderCls.getMethod("setProtectedVm", boolean.class);
            setProtected.invoke(builder, true);
            // setPayloadBinaryName 需要 APK 内已签名的 payload；本工程不含该资产。
            Method setPayload = builderCls.getMethod("setPayloadBinaryName", String.class);
            String payload = "aether_payload.so";
            if (!AvfPayload.hasPayload(ctx, payload)) {
                Log.i(AetherApp.TAG, "AVF: 无已签名 payload binary，降级 QEMU TCG");
                return false;
            }
            setPayload.invoke(builder, payload);
            Object config = builderCls.getMethod("build").invoke(builder);
            Class<?> vmCls = Class.forName("android.system.virtualmachine.VirtualMachine");
            Method getOrCreate = vmmCls.getMethod("getOrCreate", String.class,
                    Class.forName("android.system.virtualmachine.VirtualMachineConfig"));
            Object vm = getOrCreate.invoke(vmm, "aether-guest", config);
            vmCls.getMethod("run").invoke(vm);
            return true;
        } catch (Throwable t) {
            Log.i(AetherApp.TAG, "AVF 反射路径不可用，降级 QEMU TCG: " + t);
            return false;
        }
    }
}
