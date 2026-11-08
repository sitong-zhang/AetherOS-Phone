package dev.aether.vm;

/**
 * Aether VM 引擎辅助接口（自研 JNI）。
 *
 * 启动本身走 SDL2 官方入口 nativeRunMain()（在 SDLThread 上调 SDL_main），
 * 因为 Android 的 SDL 窗口/Surface 只能在那条线程上正确绑定。
 * 本类提供启动之外的引擎辅助能力：
 *  - captureStdio：把进程 stdout/stderr 重定向到文件，让 QEMU 的报错可读取；
 *  - start / stop：保留自研 core/vm_runtime.c 的 dlopen 后端，供宿主端与非 SDL
 *    场景使用；Android 端不调用，避免两条驱动链互相干扰。
 */
public final class VmEngine {
    static {
        System.loadLibrary("aethervm");
    }

    private VmEngine() {}

    /** 把 stdout/stderr 重定向到指定文件；返回 0 成功 */
    public static native int captureStdio(String path);

    /** 自研 dlopen 后端启动（Android 端不用，保留给宿主/其它场景） */
    public static native int start(String confPath);

    /** 优雅关机 */
    public static native void stop();
}
