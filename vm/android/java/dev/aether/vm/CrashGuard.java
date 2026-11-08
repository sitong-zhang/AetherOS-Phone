package dev.aether.vm;

/**
 * Native 致命信号兜底（自研）。
 * 只覆盖 SIGABRT/SIGBUS/SIGILL/SIGFPE，不碰 SIGSEGV（ART 用它做空指针检查，
 * 覆盖会破坏 JVM），并且写简报后链式转发给原 handler，不改变进程原有行为。
 * 若 native 崩溃导致 Java 弹窗来不及显示，下次冷启动会读到简报并弹报告。
 * 加载失败（例如 .so 本身缺失）时静默跳过，绝不影响 Java 崩溃监测器。
 */
public final class CrashGuard {
    private CrashGuard() {}

    public static void install(String crashDir) {
        try {
            System.loadLibrary("aethervm");
            nativeInstall(crashDir);
            BootTrace.mark("CrashGuard：native 信号兜底已注册");
        } catch (Throwable t) {
            BootTrace.mark("CrashGuard 跳过（native 兜底不可用）: " + t);
        }
    }

    private static native void nativeInstall(String crashDir);
}
