package dev.aether.vm;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Aether VM 开机入口。
 *
 * 分工：
 *  - SDL2 官方库层（org.libsdl.app.*）负责 Android 显示 surface 与触控输入，
 *    并在 SDLThread 上回调 runSDLMain() → nativeRunMain() 调引擎的 SDL_main。
 *  - 虚拟机启动参数由自研 core/vm_config.c 组装（与宿主端验证器同一份 C 源码）。
 *  - 引擎是自研交叉编译的 QEMU 8.2.0 aarch64，导出标准 SDL_main。
 *  - 系统盘/内核内置在 APK，Application 首启释放，用户零配置。
 *
 * 【1.0.5 启动阶段打点】1.0.4 修好 SDLSurface 访问权限后，SDLThread 第一次
 * 真正启动、nativeRunMain() 第一次真正进入 QEMU native 代码，真机随即闪退且
 * 零反馈：native 信号崩溃不产生 Java 异常，CrashGuard 不接管 SIGSEGV，启动
 * 标记又在 onCreate 末尾被清掉。本版在每个关键步骤 CrashHandler.markStage()
 * 覆盖写盘 —— 进程被任何信号杀死都来不及清，下次冷启动即可精确知道崩在哪一步。
 * clearLaunchStart 不再在 onCreate 末尾调用，改由「内核串口有输出＝引擎确实
 * 起来了」时清除（见 DiagnosticsOverlay.refresh）。
 */
public class BootActivity extends SDLActivity {

    private DiagnosticsOverlay diag;
    private File vmDir;
    private final AtomicBoolean engineStarted = new AtomicBoolean(false);
    private final Handler ui = new Handler(Looper.getMainLooper());

    /** 依次加载的依赖库；最后一个决定 SDL glue 去哪个 so 里找入口函数 */
    @Override
    protected String[] getLibraries() {
        BootTrace.mark("SDL glue 请求 getLibraries()：9 个 native 库");
        CrashHandler.markStage(this, "STAGE_GET_LIBRARIES");
        return new String[] {
                "compat-musl",
                "compat-limbo",
                "glib-2.0",
                "pixman-1",
                "SDL2",
                "compat-SDL2-addons",
                "compat-SDL2-ext",
                "aethervm",
                "qemu-system-aarch64"
        };
    }

    /** 引擎 .so 的绝对路径，避免 dlopen 用裸库名找不到 */
    @Override
    protected String getMainSharedObject() {
        return new File(getApplicationInfo().nativeLibraryDir,
                "libqemu-system-aarch64.so").getAbsolutePath();
    }

    /** 引擎导出的入口（QEMU main，编译期改名为 SDL_main） */
    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    /**
     * 传给引擎的 QEMU 参数，由自研 core/vm_config.c 组装，不是硬编码。
     * 返回「不含 argv[0] 的选项数组」—— nativeRunMain 会自己补一个 argv[0]。
     */
    @Override
    protected String[] getArguments() {
        BootTrace.mark("SDL glue 请求 getArguments()：转交自研 core/vm_config.c");
        CrashHandler.markStage(this, "STAGE_BUILD_ARGS");
        File conf = new File(vmDir, "vm.conf.final");
        String[] args = VmConfig.buildArgs(conf.getAbsolutePath());
        if (args == null || args.length == 0) {
            Log.e(AetherApp.TAG, "参数组装失败，返回空 argv");
            BootTrace.mark("!! 参数组装失败，argv 为空");
            CrashHandler.markStage(this, "STAGE_BUILD_ARGS_FAILED");
            return new String[0];
        }
        Log.i(AetherApp.TAG, "Aether VM 启动参数 " + args.length + " 项: "
                + VmConfig.summary(conf.getAbsolutePath()));
        BootTrace.mark("参数组装完成：" + args.length + " 项（不含 argv[0]）");
        return args;
    }

    /** SDL2 库层在 SDLThread 上回调的钩子：真正把 QEMU 跑起来 */
    @Override
    protected void runSDLMain() {
        engineStarted.set(true);
        BootTrace.mark("SDLThread 回调 runSDLMain()（线程 "
                + Thread.currentThread().getName() + "）");
        CrashHandler.markStage(this, "STAGE_SDLMAIN_CALLBACK thread="
                + Thread.currentThread().getName());

        // SDLMain.run() 结尾若 mExitCalledFromJava=false 会调 handleNativeExit()
        // → mSingleton.finish()，QEMU 一旦启动失败退出，界面立刻消失，
        // 用户根本来不及复制诊断报告。这里置 true，让界面留着显示引擎报错。
        SDLActivity.mExitCalledFromJava = true;

        String lib = getMainSharedObject();
        String fn  = getMainFunction();
        String[] args = getArguments();

        // ★ 关键打点：下一行之后就进入 QEMU native 代码，若进程在此后崩溃，
        //   阶段文件会停在 STAGE_NATIVE_RUNMAIN_ENTER，冷启动据此定位。
        CrashHandler.markStage(this, "STAGE_NATIVE_RUNMAIN_ENTER fn=" + fn
                + " lib=" + lib + " argc=" + (args.length + 1));
        BootTrace.mark("调用 nativeRunMain: fn=" + fn + " argc=" + (args.length + 1));
        if (diag != null) diag.setEngineState("引擎：nativeRunMain 已调用，正在启动 QEMU …");

        int rc;
        try {
            rc = SDLActivity.nativeRunMain(lib, fn, args);
        } catch (Throwable t) {
            BootTrace.mark("!! nativeRunMain 抛异常 " + t);
            CrashHandler.markStage(this, "STAGE_NATIVE_RUNMAIN_THREW " + t);
            CrashHandler.saveException(this, t, "nativeRunMain");
            Log.e(AetherApp.TAG, "nativeRunMain 失败", t);
            if (diag != null) diag.setEngineState("引擎启动异常: " + t + "；请点保存文件");
            return;
        }
        CrashHandler.markStage(this, "STAGE_NATIVE_RUNMAIN_RETURN rc=" + rc);
        BootTrace.mark("nativeRunMain 返回 " + rc + "（引擎主循环结束）");
        if (diag != null) diag.setEngineState("引擎已退出，返回码 " + rc
                + "；请查看下方引擎输出并点保存文件");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        BootTrace.mark("BootActivity.onCreate 进入");
        CrashHandler.markStage(this, "STAGE_BOOT_ONCREATE_ENTER");
        vmDir = new File(getFilesDir(), "vm");
        if (AetherApp.vmDir == null) AetherApp.vmDir = vmDir;

        // 把 QEMU 的 stdout/stderr 落盘，诊断浮层才能显示引擎报错
        int cap = VmEngine.captureStdio(new File(vmDir, "qemu_stdio.log").getAbsolutePath());
        BootTrace.mark("引擎输出重定向到 qemu_stdio.log (rc=" + cap + ")");
        CrashHandler.markStage(this, "STAGE_STDIO_CAPTURED rc=" + cap);

        BootTrace.mark("调用 super.onCreate(SDLActivity)：加载 native 库 + SDL 状态初始化");
        CrashHandler.markStage(this, "STAGE_SDL_SUPER_ONCREATE_BEFORE (载入 9 个 native 库)");
        super.onCreate(savedInstanceState);
        CrashHandler.markStage(this, "STAGE_SDL_SUPER_ONCREATE_DONE (native 库已载入)");

        // 诊断浮层先 attach：后续 Surface / 引擎初始化万一抛错，屏幕上仍有确切
        // 原因和「保存文件」，而不是只剩一个崩溃弹窗（1.0.4 的教训）。
        diag = new DiagnosticsOverlay(this, vmDir);
        diag.attach();
        diag.setEngineState("引擎：等待 SDL 线程启动 …");
        CrashHandler.markStage(this, "STAGE_DIAG_ATTACHED");

        // ===== 补上库层缺失的显示 Surface =====
        // mSurface 必须指向真实 SDLSurface：SDL native 层靠
        // SDLActivity.getNativeSurface() → mSurface.getNativeSurface() 拿 ANativeWindow，
        // 否则 -display sdl 建不出窗口；surfaceChanged 也永远不会调 handleNativeState()。
        final SDLSurface surface;
        try {
            surface = new SDLSurface(this);
            SDLActivity.mSurface = surface;
            FrameLayout layout = new FrameLayout(this);
            layout.addView(surface, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            SDLActivity.mLayout = layout;
            setContentView(layout);
        } catch (Throwable t) {
            BootTrace.mark("!! SDL Surface 创建失败: " + t);
            CrashHandler.markStage(this, "STAGE_SURFACE_CREATE_FAILED " + t);
            CrashHandler.saveException(this, t, "SDLSurface 创建");
            Log.e(AetherApp.TAG, "SDL Surface 创建失败", t);
            diag.setEngineState("显示 Surface 创建失败: " + t + "；请点保存文件");
            return;
        }
        BootTrace.mark("SDL Surface 已创建并 setContentView（补齐 glue 缺失的一环）");
        CrashHandler.markStage(this, "STAGE_SURFACE_READY");

        if (SDLActivity.mBrokenLibraries) {
            BootTrace.mark("!! SDL glue 报告 native 库加载失败（mBrokenLibraries=true）");
            CrashHandler.markStage(this, "STAGE_BROKEN_LIBRARIES");
        } else {
            BootTrace.mark("SDL glue：9 个 native 库加载成功，setupJNI / mSingleton 就绪");
        }

        // surface 就绪检测 + 兜底。glue 的 SDLSurface 自己已 addCallback(this)，
        // 我们再挂一个只读 Callback 观察生命周期，不改动它原来的状态机。
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {
                BootTrace.mark("SDLSurface.surfaceCreated（ANativeWindow 就绪）");
                CrashHandler.markStage(BootActivity.this, "STAGE_SURFACE_CREATED_CALLBACK");
                scheduleEngineFallback();
            }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int ht) {
                BootTrace.mark("SDLSurface.surfaceChanged " + w + "x" + ht);
                CrashHandler.markStage(BootActivity.this,
                        "STAGE_SURFACE_CHANGED " + w + "x" + ht);
                scheduleEngineFallback();
            }
            @Override public void surfaceDestroyed(SurfaceHolder h) {
                BootTrace.mark("SDLSurface.surfaceDestroyed");
            }
        });

        // ★ 不再在这里 clearLaunchStart：崩溃发生在 onCreate 之后的 SDLThread，
        //   过早清除会让冷启动兜底误判为「正常」。改由内核串口出现输出时清除。
        CrashHandler.markStage(this, "STAGE_BOOT_ONCREATE_DONE (等待 SDLThread)");
        BootTrace.mark("BootActivity.onCreate 完成（SDL Surface + 诊断浮层就绪）");
    }

    /**
     * 兜底：若 3 秒内 SDL glue 的状态机（需要同时满足 surfaceReady / hasFocus /
     * resumedCalled / RESUMED 四个条件）没拉起 SDLThread，自研代码启动唯一一次。
     * 把 mSDLThread 一并赋值，glue 后续就不会再重复启动第二条 QEMU。
     */
    private void scheduleEngineFallback() {
        ui.postDelayed(new Runnable() {
            @Override public void run() { fallbackCheck(); }
        }, 3000);
    }

    /**
     * 放在外部类方法体里读写 glue 的静态字段：javac 在匿名内部类里会把继承来的
     * 静态字段编译成 Ldev/aether/vm/BootActivity;->mSDLThread（owner 是子类），
     * 挪出来后 dex 符号引用直接指向 Lorg/libsdl/app/SDLActivity;->mSDLThread，
     * 运行时解析无歧义。
     */
    private void fallbackCheck() {
        if (engineStarted.get()) return;
        if (SDLActivity.mSDLThread != null) {
            BootTrace.mark("SDL glue 已启动 SDLThread（无需兜底）");
            return;
        }
        BootTrace.mark("!! SDL glue 未拉起线程，自研兜底启动引擎");
        CrashHandler.markStage(this, "STAGE_FALLBACK_START_ENGINE");
        if (!engineStarted.compareAndSet(false, true)) return;
        Thread t = new Thread(new Runnable() {
            @Override public void run() { runSDLMain(); }
        }, "SDLThread");
        SDLActivity.mSDLThread = t;
        t.start();
    }
}
