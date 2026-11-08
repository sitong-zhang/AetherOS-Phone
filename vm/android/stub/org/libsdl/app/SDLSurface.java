package org.libsdl.app;

/*
 * 编译期占位类：仅供 javac 解析 BootActivity 里对 SDL2 库层字段/类型的引用。
 * 不打包进 APK；运行时由 classes2.dex 里的 SDL2 官方库层
 * org/libsdl/app/SDLSurface.smali 提供真实实现（SurfaceView + Holder.Callback
 * + 触控/传感器 + getNativeSurface）。
 *
 * 类型描述符必须与真实类一致：Lorg/libsdl/app/SDLSurface;
 */
import android.content.Context;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public class SDLSurface extends SurfaceView implements SurfaceHolder.Callback {

    public SDLSurface(Context c) { super(c); }

    /** 真实实现返回 getHolder().getSurface()，供 SDL native 层取 ANativeWindow */
    public android.view.Surface getNativeSurface() {
        return getHolder().getSurface();
    }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int ht) {}
    @Override public void surfaceDestroyed(SurfaceHolder h) {}
}
