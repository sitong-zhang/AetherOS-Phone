package dev.aether.vm;

/**
 * 启动参数由自研 core/vm_config.c 组装，这里只是 JNI 桥。
 * 与宿主端验证器用的是同一份 C 源码，不是两套参数。
 */
public final class VmConfig {
    static {
        System.loadLibrary("aethervm");
    }

    private VmConfig() {}

    /** 返回 QEMU argv（不含 argv[0]），供 SDL glue 的 getArguments() 使用 */
    public static native String[] buildArgs(String confPath);

    /** 返回人类可读的虚拟机摘要 */
    public static native String summary(String confPath);
}
