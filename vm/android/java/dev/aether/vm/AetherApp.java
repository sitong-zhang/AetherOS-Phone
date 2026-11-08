package dev.aether.vm;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetManager;
import android.system.Os;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Aether VM 应用入口。
 *
 * 崩溃监测器在 attachBaseContext() 就装载（比 onCreate 更早），因此启动阶段
 * 任何 Java 异常都会被捕获并弹窗展示。:crash 独立进程只是展示报告的弹窗，
 * 不需要释放 41MB 内核资产，这里按进程名跳过。
 */
public class AetherApp extends Application {
    public static final String TAG = "AetherVM";
    public static File vmDir;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        if (!isCrashProcess()) {
            CrashHandler.install(this);       // 最早时机：覆盖 Application 初始化及之后全部崩溃
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();

        if (isCrashProcess()) {
            BootTrace.mark("AetherApp.onCreate 在 :crash 进程，跳过资产释放");
            return;                            // 弹窗进程不重复释放内核/系统盘
        }

        BootTrace.mark("AetherApp.onCreate（主进程）");
        vmDir = new File(getFilesDir(), "vm");
        try {
            installAssets();
            BootTrace.mark("资产释放完成：kernel.img / system.qcow2 / vm.conf.final");
        } catch (IOException e) {
            BootTrace.mark("资产释放失败: " + e);
            Log.e(TAG, "资产释放失败", e);
        }

        /* 1.0.11：把真实屏幕尺寸注入进程环境，供 QEMU SDL2 引擎决定窗口与
         * 贴图尺寸。Android 上 SDL_GetWindowSize / SDL_GetDesktopDisplayMode
         * 有可能只返回 guest 的小尺寸，这个注入值是唯一确定的来源。 */
        try {
            DisplayMetrics dm = new DisplayMetrics();
            WindowManager wm = (WindowManager) getSystemService(Context.WINDOW_SERVICE);
            if (wm != null) {
                wm.getDefaultDisplay().getMetrics(dm);
                String screen = dm.widthPixels + "x" + dm.heightPixels;
                Os.setenv("AETHER_SCREEN", screen, true);
                BootTrace.mark("AETHER_SCREEN 注入 " + screen);
            }
        } catch (Throwable t) {
            BootTrace.mark("AETHER_SCREEN 注入失败: " + t);
        }

        /* 冷启动统一合成（1.0.5）：把 native 静默死亡 / 启动未完成 / native 信号
         * 简报合并成一份可展示报告。包 try/catch，绝不让合成逻辑拖垮 Application。
         * LaunchActivity 会再调一次（幂等），确保即使 Application 阶段异常也有报告。 */
        try {
            CrashHandler.synthesizeOnColdStart(this);
        } catch (Throwable t) {
            Log.e(TAG, "冷启动报告合成异常（已忽略）", t);
        }
        BootTrace.mark("AetherApp.onCreate 完成（冷启动报告合成已执行）");
    }

    private boolean isCrashProcess() {
        String p = CrashHandler.readCmdline();
        return p != null && p.endsWith(":crash");
    }

    private void installAssets() throws IOException {
        if (!vmDir.exists() && !vmDir.mkdirs())
            throw new IOException("无法创建 " + vmDir);

        String[] names = {"kernel.img", "system.qcow2"};
        AssetManager am = getAssets();
        for (String n : names) {
            File dst = new File(vmDir, n);
            File stamp = new File(vmDir, "." + n + ".ok");
            if (dst.exists() && stamp.exists()) continue;   // 已释放过
            BootTrace.mark("释放资产 " + n);
            try (InputStream in = am.open(n); OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[64 * 1024];
                int r;
                while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
            }
            new FileOutputStream(stamp).close();
            Log.i(TAG, "释放 " + n + " -> " + dst.length() + " bytes");
        }

        // 生成最终配置：把 @KERNEL@ / @DISK@ / @RUN@ 换成私有目录绝对路径
        StringBuilder sb = new StringBuilder();
        try (InputStream in = am.open("vm.conf")) {
            byte[] buf = new byte[4096];
            int r;
            while ((r = in.read(buf)) > 0) sb.append(new String(buf, 0, r, StandardCharsets.UTF_8));
        }
        String conf = sb.toString()
                .replace("@KERNEL@", new File(vmDir, "kernel.img").getAbsolutePath())
                .replace("@DISK@",   new File(vmDir, "system.qcow2").getAbsolutePath())
                .replace("@RUN@",    vmDir.getAbsolutePath());
        try (FileOutputStream out = new FileOutputStream(new File(vmDir, "vm.conf.final"))) {
            out.write(conf.getBytes(StandardCharsets.UTF_8));
        }
        Log.i(TAG, "虚拟机配置已生成: " + new File(vmDir, "vm.conf.final"));
    }
}
