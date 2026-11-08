package org.libsdl.app;

/* 仅供 javac 解析 BootActivity 父类方法/字段签名用，不打包进 APK。
 * 运行时由 SDL2 官方库层 org/libsdl/app/*.smali 提供真实实现。 */
import android.app.Activity;
import android.os.Bundle;
import android.view.ViewGroup;

public class SDLActivity extends Activity {

    /* ---- SDL glue 的静态状态（真实字段在 classes2.dex 里）----
     * BootActivity 需要显式赋值 mSurface / mLayout：Limbo 版 glue 的 onCreate()
     * 只做 setupJNI / initialize / mSingleton，SurfaceView 由业务 Activity 创建，
     * 独立工程缺了这一环就会让 mIsSurfaceReady 永远是 false，SDLThread 永不启动。 */
    protected static SDLSurface mSurface;
    protected static ViewGroup  mLayout;
    protected static Thread     mSDLThread;
    public    static boolean    mBrokenLibraries;
    public    static boolean    mExitCalledFromJava;

    protected String[] getLibraries() { return new String[0]; }
    protected String getMainFunction() { return "SDL_main"; }
    protected String[] getArguments() { return new String[0]; }
    protected String getMainSharedObject() { return "libmain.so"; }
    protected void runSDLMain() {}
    protected void onCreate(Bundle b) { super.onCreate(b); }

    /* SDL2 官方入口：在 SDLThread 里 dlopen 引擎、组装 argv 并调用 SDL_main。
     * 实现在 libSDL2.so：Java_org_libsdl_app_SDLActivity_nativeRunMain */
    public static native int nativeRunMain(String library, String function, Object arguments);
}
