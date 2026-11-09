package dev.aether.vm;

import android.content.Context;

import java.io.IOException;
import java.util.Arrays;

/**
 * AVF payload 资产检查。
 *
 * AOSP VirtualMachineConfig.Builder.setPayloadBinaryName() 要求 payload 位于
 * APK 的 lib/<abi>/ 下并随 APK 签名。本工程交付的是完整 Linux 内核(kernel.img)
 * 与系统盘(system.qcow2)，不是 Microdroid payload；因此默认返回 false，AVF
 * 实验路径不会误启动。未来若提供系统签名版并内置 payload，把资产放进
 * assets/avf/ 并返回 true 即可启用，无需改启动决策代码。
 */
public final class AvfPayload {

    private AvfPayload() {}

    /** 只读检查 APK 资产里是否存在同名 payload；不存在即返回 false。 */
    public static boolean hasPayload(Context ctx, String binaryName) {
        try {
            String[] list = ctx.getAssets().list("avf");
            if (list == null) return false;
            return Arrays.asList(list).contains(binaryName);
        } catch (IOException e) {
            return false;
        }
    }
}
