package dev.aether.vm;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Process;
import android.util.Log;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * Aether VM 全局崩溃监测器（自研）。
 *
 * 在 Application.attachBaseContext() 就装载，比 onCreate 更早，因此能抓到
 * 资产释放、System.loadLibrary、Activity 初始化等启动阶段的任何 Java 异常
 * （UnsatisfiedLinkError / NoClassDefFoundError / IllegalAccessError …）。
 *
 * 【1.0.5 零反馈死循环修复】1.0.4 把 SDLSurface 改回 public 后，Surface 第一次
 * 真正创建成功 → SDLThread 第一次真正启动 → nativeRunMain() 第一次真正进入
 * QEMU native 代码。QEMU 在真机上崩在这一层时，旧版三重兜底全部落空：
 *   1) native 信号崩溃不产生 Java 异常，UncaughtExceptionHandler 抓不到；
 *   2) CrashGuard 刻意不接管 SIGSEGV（覆盖会破坏 ART 的隐式空指针检查）；
 *   3) launch_started.flag 在 BootActivity.onCreate 末尾就被清掉，而崩溃发生
 *      在其后的 SDLThread 里 → hasUnfinishedLaunch() 判为「正常」。
 * 结果：每次点开都闪退、每次下次启动都判定无需报告 —— 死循环零反馈。
 *
 * 本版引入【启动阶段标记 boot_stage.txt】：每走一步覆盖写盘，进程被任何信号
 * 杀死都来不及清；下次冷启动 synthesizeOnColdStart() 读它合成报告，并把
 * QEMU stdout/stderr、内核串口、logcat crash buffer(native tombstone) 一并纳入。
 * 报告由 LaunchActivity 内联显示，不依赖可能被 ROM 拦截的跨进程弹窗。
 */
public final class CrashHandler implements Thread.UncaughtExceptionHandler {
    private static final String TAG = "AetherVM/CRASH";
    public static final String EXTRA_TEXT = "dev.aether.vm.CRASH_TEXT";

    private final Context app;
    private final Thread.UncaughtExceptionHandler previous;

    private static File sDir, sLatest, sPending, sNative, sStage;

    private CrashHandler(Context c, Thread.UncaughtExceptionHandler p) {
        app = c;
        previous = p;
    }

    /* ---------------- 路径 ---------------- */
    public static File dir(Context c) {
        if (sDir == null) {
            sDir = new File(c.getFilesDir(), "crash");
            if (!sDir.exists()) sDir.mkdirs();
            sLatest  = new File(sDir, "latest.txt");
            sPending = new File(sDir, "pending.flag");
            sNative  = new File(sDir, "native_crash.txt");
            sStage   = new File(sDir, "boot_stage.txt");
        }
        return sDir;
    }

    public static File pendingFile(Context c) { dir(c); return sPending; }
    public static File nativeFile(Context c)  { dir(c); return sNative; }
    public static File stageFile(Context c)   { dir(c); return sStage; }

    /* ---------------- 启动未完成兜底（兼容旧语义）---------------- */
    public static File launchFlag(Context c) {
        dir(c);
        return new File(sDir, "launch_started.flag");
    }

    public static void markLaunchStart(Context c) {
        try {
            write(launchFlag(c), stamp());
        } catch (Throwable ignored) {}
    }

    public static void clearLaunchStart(Context c) {
        try { launchFlag(c).delete(); } catch (Throwable ignored) {}
    }

    public static boolean hasUnfinishedLaunch(Context c) {
        return launchFlag(c).exists();
    }

    /* ---------------- 启动阶段标记（1.0.5 核心）----------------
     * 覆盖写，只保留「最后走到哪一步」。进程被 SIGSEGV/SIGABRT/LMK 杀死时
     * 文件已经落盘且不会被清，下次冷启动即可精确判断崩溃阶段。 */
    public static void markStage(Context c, String stage) {
        try { write(stageFile(c), stamp() + "  " + stage + "\n"); }
        catch (Throwable ignored) {}
    }

    public static String readStage(Context c) {
        try { return readFile(stageFile(c)).trim(); }
        catch (Throwable t) { return "(无阶段记录)"; }
    }

    public static void clearStage(Context c) {
        try { stageFile(c).delete(); } catch (Throwable ignored) {}
    }

    public static boolean hasStage(Context c) { return stageFile(c).exists(); }

    /**
     * 冷启动统一合成：把所有「静默死亡」信号并在一份可复制报告里。幂等，
     * 且整体包 try/catch —— 绝不让合成逻辑把 Application 启动拖崩。
     * 在 AetherApp.onCreate 与 LaunchActivity.onCreate 各调一次。
     */
    public static void synthesizeOnColdStart(Context c) {
        try {
            if (hasPending(c)) return;          // 已有 Java 异常报告，优先展示它
            File vm = new File(c.getFilesDir(), "vm");
            String serial = safeTail(new File(vm, "serial.log"), 4000);

            String brief;
            if (nativeFile(c).exists()) {
                brief = "上次发生 native 致命信号（Java 弹窗未及显示）：\n"
                      + readNative(c) + "\n";
            } else if (hasStage(c) || hasUnfinishedLaunch(c)) {
                boolean engineUp = !serial.startsWith("(") && serial.trim().length() > 0;
                brief = "【上次启动未正常收尾 · 零反馈根因】\n"
                      + "最后停留阶段: " + readStage(c) + "\n"
                      + "进程在开机流程中被终止，没有产生 Java 异常，所以旧版的崩溃弹窗、\n"
                      + "诊断浮层、启动未完成兜底都没能显示任何信息。\n"
                      + "内核串口: " + (engineUp
                            ? "有输出 → 引擎曾启动，之后进程终止"
                            : "无输出 → 引擎未成功启动，崩在 QEMU/SDL native 初始化阶段") + "\n"
                      + "常见原因：native 致命信号(SIGSEGV/SIGABRT)、内存不足被系统回收(LMK)、\n"
                      + "          -display sdl / virtio-gpu 初始化失败、QEMU 参数不被真机接受。\n"
                      + "★ 本文件为完整报告，已自动保存为 .txt；点「保存文件」即可通过\n"
                      + "  系统分享把原文件发给开发者，不会因粘贴被截断。\n"
                      + "  【① 引擎 stdout/stderr ② 内核串口 ③ native 崩溃摘要】置顶，\n"
                      + "  是区分「QEMU 参数/初始化报错 exit」与「native 信号崩溃」的关键。\n";
            } else {
                return;                         // 上次正常收尾，无需报告
            }

            String report = buildReport(c, null, null, brief);
            try { save(report); } catch (Throwable ignored) {}
            // 1.0.7：报告合成瞬间就自动落盘一份完整 .txt —— native 秒崩时 UI
            // 可能来不及停留，用户仍能直接在「下载/AetherVM/」拿到原文件。
            try { ReportSaver.save(c, report); } catch (Throwable ignored) {}
            // 清掉所有「未完成」痕迹，避免每次冷启动重复弹同一份
            clearStage(c);
            clearLaunchStart(c);
            try { if (nativeFile(c).exists()) nativeFile(c).delete(); } catch (Throwable ignored) {}
        } catch (Throwable t) {
            Log.e(TAG, "冷启动报告合成失败（已忽略，不影响启动）", t);
        }
    }

    /* ---------------- 安装（最早时机）---------------- */
    public static void install(Context c) {
        File d = dir(c);
        BootTrace.init(d);
        BootTrace.mark("CrashHandler.install：全局崩溃监测器已装载");
        Thread.UncaughtExceptionHandler cur = Thread.getDefaultUncaughtExceptionHandler();
        if (!(cur instanceof CrashHandler))
            Thread.setDefaultUncaughtExceptionHandler(new CrashHandler(c, cur));
        CrashGuard.install(d.getAbsolutePath());
    }

    public static boolean hasPending(Context c) { return pendingFile(c).exists(); }

    /** 被 try/catch 吞掉的致命异常也要留一份报告，否则冷启动只能靠阶段标记推断 */
    public static void saveException(Context c, Throwable e, String where) {
        try {
            String report = buildReport(c, Thread.currentThread(), e,
                    "代码内捕获到的致命异常（位置: " + where + "）。\n");
            save(report);
            try { ReportSaver.save(c, report); } catch (Throwable ignored) {}
            BootTrace.mark("!! 已就「" + where + "」生成报告并自动落盘");
        } catch (Throwable ignored) {}
    }

    /* ---------------- 崩溃入口 ---------------- */
    @Override
    public void uncaughtException(Thread thread, Throwable ex) {
        BootTrace.mark("!! 未捕获异常 " + ex.getClass().getName() + ": " + ex.getMessage());
        final String report = buildReport(app, thread, ex, null);
        try { save(report); } catch (Throwable ignored) {}
        try { ReportSaver.save(app, report); } catch (Throwable ignored) {}
        clearLaunchStart(app);
        clearStage(app);        // 已有精确 Java 异常报告，别让下次冷启动再合成一份模糊的
        try { Log.e(TAG, "\n" + report); } catch (Throwable ignored) {}
        showReport(app, report);

        /* 给 binder 一点时间把启动弹窗的 Intent 送出去，然后主动结束进程 */
        try { Thread.sleep(700); } catch (InterruptedException ignored) {}
        Process.killProcess(Process.myPid());
        System.exit(10);
    }

    /* ---------------- 报告 ---------------- */
    static String buildReport(Context c, Thread t, Throwable e, String nativeBrief) {
        StringBuilder b = new StringBuilder();
        b.append("==================== Aether VM 崩溃报告 ====================\n");
        b.append("生成时间: ").append(stamp()).append("\n\n");

        b.append("---- 应用 / 设备 ----\n").append(deviceInfo(c)).append("\n");

        if (nativeBrief != null)
            b.append("---- 上次崩溃判定 ----\n").append(nativeBrief).append("\n");

        b.append("---- 异常 ----\n");
        if (e != null) {
            b.append("线程: ").append(t == null ? "unknown" : t.getName())
             .append(" (id=").append(t == null ? -1 : t.getId()).append(")\n");
            StringWriter sw = new StringWriter();
            PrintWriter pw = new PrintWriter(sw);
            e.printStackTrace(pw);          // 自动包含 Caused by / Suppressed 链
            pw.flush();
            b.append(sw.toString()).append("\n");
        } else {
            b.append("(本次/上次未捕获到 Java 异常；请看下面的阶段与引擎日志)\n\n");
        }

        File vm = new File(c.getFilesDir(), "vm");

        // ★ 1.0.7：报告现在整份落盘成 .txt，不再受聊天框粘贴长度限制。
        // 三段仍置顶（阅读方便），但下面各段上限全面放宽：文件里是完整原文，
        // 不截断、不「优先发三段」—— 直接把整个 .txt 文件发给开发者即可。
        b.append("【说明】本文件为完整报告（未做任何粘贴长度裁剪）。\n");
        b.append("点「保存文件」把本 .txt 通过系统分享发给开发者即可，不会有缺失。\n\n");

        b.append("========== ★ ① 引擎 stdout/stderr（qemu_stdio.log）==========\n")
         .append(describeTail(new File(vm, "qemu_stdio.log"), 200000)).append("\n");
        b.append("========== ★ ② 内核串口 serial.log ==========\n")
         .append(describeTail(new File(vm, "serial.log"), 200000)).append("\n");
        b.append("========== ★ ③ native 崩溃摘要（Fatal signal / backtrace / tombstone）==========\n")
         .append(nativeCrashDigest()).append("\n");
        b.append("========== ★ 以上三段最重要；以下为完整补充信息 ==========\n\n");

        b.append("---- 完整 logcat（main/system/crash）----\n")
         .append(collectLogcat()).append("\n");
        b.append("---- 最后停留阶段 ----\n").append(readStage(c)).append("\n\n");
        b.append("---- 运行配置 vm.conf.final ----\n")
         .append(safeTail(new File(vm, "vm.conf.final"), 16384)).append("\n");
        b.append("---- 打包环境自检 ----\n").append(envSelfCheck(c)).append("\n");
        b.append("---- 启动进度 BootTrace（仅保留最近 2 次启动）----\n")
         .append(clip(BootTrace.read(), 60000)).append("\n");
        b.append("==================== 报告结束：直接把本 .txt 文件发给开发者 ====================\n");
        return b.toString();
    }

    private static String deviceInfo(Context c) {
        StringBuilder b = new StringBuilder();
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            b.append("应用: ").append(pi.packageName)
             .append("  versionName=").append(pi.versionName)
             .append("  versionCode=").append(pi.versionCode).append('\n');
            b.append("APK: ").append(c.getApplicationInfo().sourceDir).append('\n');
        } catch (Throwable t) {
            b.append("应用信息读取失败: ").append(t).append('\n');
        }
        b.append("设备: ").append(Build.MANUFACTURER).append(" / ").append(Build.BRAND)
         .append(" / ").append(Build.MODEL).append('\n');
        b.append("系统: Android ").append(Build.VERSION.RELEASE)
         .append(" (API ").append(Build.VERSION.SDK_INT).append(")  ")
         .append(Build.DISPLAY).append('\n');
        b.append("指纹: ").append(Build.FINGERPRINT).append('\n');
        b.append("ABI : ").append(Arrays.toString(Build.SUPPORTED_ABIS))
         .append("  CPU_ABI=").append(Build.CPU_ABI).append('\n');
        b.append("进程: ").append(readCmdline()).append("  pid=").append(Process.myPid()).append('\n');
        Runtime r = Runtime.getRuntime();
        b.append("内存: heapMax=").append(r.maxMemory() / 1048576)
         .append("MB  heapUsed=").append((r.totalMemory() - r.freeMemory()) / 1048576)
         .append("MB\n");
        return b.toString();
    }

    private static String envSelfCheck(Context c) {
        StringBuilder b = new StringBuilder();
        try {
            ApplicationInfo ai = c.getApplicationInfo();
            b.append("nativeLibraryDir: ").append(ai.nativeLibraryDir).append('\n');
            File d = new File(ai.nativeLibraryDir);
            File[] fs = d.listFiles();
            if (fs == null) {
                b.append("  (目录不可读)\n");
            } else {
                Arrays.sort(fs);
                for (File f : fs)
                    b.append("  ").append(f.getName()).append("  ").append(f.length()).append(" B\n");
            }
        } catch (Throwable t) {
            b.append("nativeLibraryDir 自检失败: ").append(t).append('\n');
        }
        try {
            String[] a = c.getAssets().list("");
            b.append("assets: ").append(Arrays.toString(a)).append('\n');
        } catch (Throwable t) {
            b.append("assets 自检失败: ").append(t).append('\n');
        }
        return b.toString();
    }

    /** 带字节数标注地读日志尾：区分「无文件」「空文件 0 B」「有内容」，避免被误读成没采集 */
    private static String describeTail(File f, int max) {
        if (f == null || !f.exists())
            return "(无文件：引擎尚未创建该日志；QEMU 若在 qemu_init 前秒崩属正常)\n";
        long len = f.length();
        if (len == 0)
            return "(空文件 0 B：引擎没有写入任何输出；请结合下面 ③ native 摘要判断)\n";
        return "[" + len + " B]\n" + safeTail(f, max);
    }

    private static String clip(String s, int max) {
        if (s == null) return "(null)\n";
        return s.length() <= max ? s : "…(更早略)\n" + s.substring(s.length() - max);
    }

    /**
     * 【1.0.6】把 native 崩溃的关键行单独抽出来置顶，不受主 logcat 行数或
     * 用户粘贴截断影响。用于区分两种「进程未正常收尾」：
     *   - QEMU 参数/初始化失败 → error_report 到 stderr 后 exit(1)，① 里有报错；
     *   - native 信号崩溃      → crash buffer 里有 Fatal signal + backtrace。
     */
    private static String nativeCrashDigest() {
        String crash = dumpLogcat(new String[]{"logcat", "-d", "-b", "crash",
                "-t", "3000", "-v", "threadtime"}, 500000);
        String sys = dumpLogcat(new String[]{"logcat", "-d", "-b", "main,system",
                "-t", "3000", "-v", "threadtime"}, 500000);
        StringBuilder hit = new StringBuilder();
        for (String l : (crash + "\n" + sys).split("\n")) {
            if (l.contains("Fatal signal") || l.contains("SIGSEGV") || l.contains("SIGABRT")
                || l.contains("SIGBUS") || l.contains("backtrace")
                || l.contains("Abort message") || l.contains("tombstone")
                || l.contains("libqemu-system") || l.contains("libSDL2")
                || l.contains("libaethervm") || l.contains("AetherVM")
                || l.contains("nativeRunMain") || l.contains("DEBUG")
                || (l.contains(" pc ") && l.trim().startsWith("#"))) {
                hit.append(l).append('\n');
            }
        }
        if (hit.length() == 0) {
            return "(未在 logcat 检出 native Fatal signal / backtrace。\n"
                 + " 判别：① 若上面 ① 引擎 stdout/stderr 有 QEMU 报错 → 参数/初始化失败 exit(1)；\n"
                 + "       ② 若 ① 也为空 → 可能本 ROM 禁止读 logcat，或进程被 LMK 静默杀死；\n"
                 + "       ③ 若崩在 dlopen/dlsym，可能只留下阶段标记而无任何日志。)\n";
        }
        return hit.toString();
    }

    /** 多 buffer 抓 logcat：crash buffer 里有 native Fatal signal / tombstone backtrace */
    private static String collectLogcat() {
        String s = dumpLogcat(new String[]{"logcat", "-d", "-b", "main,system,crash",
                "-t", "3000", "-v", "threadtime"}, 500000);
        if (s.trim().isEmpty() || s.contains("读取失败"))
            s = dumpLogcat(new String[]{"logcat", "-d", "-t", "3000", "-v", "threadtime"}, 500000);
        return s.trim().isEmpty() ? "(logcat 无输出或本 ROM 禁止读取)\n" : s;
    }

    private static String dumpLogcat(String[] cmd, int maxBytes) {
        StringBuilder b = new StringBuilder();
        java.lang.Process p = null;
        BufferedReader br = null;
        try {
            p = Runtime.getRuntime().exec(cmd);
            br = new BufferedReader(
                    new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8), 8192);
            String line;
            int bytes = 0;
            while ((line = br.readLine()) != null) {
                b.append(line).append('\n');
                bytes += line.length() + 1;
                if (bytes > maxBytes) { b.append("...(日志已截断)\n"); break; }
            }
        } catch (Throwable t) {
            b.append("(logcat 读取失败: ").append(t).append(")\n");
        } finally {
            if (br != null) try { br.close(); } catch (Throwable ignored) {}
            if (p != null)  try { p.destroy(); } catch (Throwable ignored) {}
        }
        return b.toString();
    }

    /* ---------------- 持久化 / 读取 ---------------- */
    private static void save(String report) throws java.io.IOException {
        write(sLatest, report);
        write(sPending, "1");
    }

    public static String readLatest(Context c) {
        dir(c);
        if (!sLatest.exists()) return "(未找到崩溃报告文件)";
        try { return readFile(sLatest); }
        catch (Throwable t) { return "(读取报告失败: " + t + ")"; }
    }

    /** 冷启动兜底：把 native 信号简报转成一份可展示的崩溃报告（保留供兼容） */
    public static void composeFromNative(Context c) {
        dir(c);
        String brief = "(native_crash.txt 为空)";
        try { if (sNative.exists()) brief = readFile(sNative); } catch (Throwable ignored) {}
        String report = buildReport(c, null, null, brief);
        try { save(report); } catch (Throwable ignored) {}
        try { if (sNative.exists()) sNative.delete(); } catch (Throwable ignored) {}
    }

    public static void composeFromUnfinished(Context c) {
        String brief = "上次启动未完成（标记时间见下）。\n"
                     + "最后停留阶段: " + readStage(c) + "\n";
        String report = buildReport(c, null, null, brief);
        try { save(report); } catch (Throwable ignored) {}
        clearLaunchStart(c);
        clearStage(c);
    }

    public static void showReport(Context c, String text) {
        try {
            Intent i = new Intent();
            i.setComponent(new ComponentName(c.getPackageName(),
                    "dev.aether.vm.CrashReportActivity"));
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_CLEAR_TASK
                    | Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS);
            i.putExtra(EXTRA_TEXT, text == null ? readLatest(c) : text);
            c.startActivity(i);
        } catch (Throwable t) {
            Log.e(TAG, "启动崩溃弹窗失败", t);
        }
    }

    private static String stamp() {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA).format(new Date());
    }

    private static String readNative(Context c) {
        try { return readFile(nativeFile(c)); }
        catch (Throwable t) { return "(native_crash.txt 读取失败)"; }
    }

    private static String safeTail(File f, int max) {
        if (f == null || !f.exists()) return "(无文件)\n";
        try {
            String s = readFile(f);
            if (s.length() <= max) return s;
            return "…(前略)\n" + s.substring(s.length() - max);
        } catch (Throwable t) {
            return "(读取失败: " + t + ")\n";
        }
    }

    private static void write(File f, String s) throws java.io.IOException {
        FileOutputStream out = null;
        try {
            out = new FileOutputStream(f, false);
            out.write(s.getBytes(StandardCharsets.UTF_8));
            out.flush();
        } finally {
            if (out != null) try { out.close(); } catch (Throwable ignored) {}
        }
    }

    public static String readFile(File f) throws java.io.IOException {
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
            return new String(bos.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }

    /** 读 /proc/self/cmdline 得到进程名，用于区分主进程与 :crash 弹窗进程 */
    public static String readCmdline() {
        FileInputStream in = null;
        try {
            in = new FileInputStream("/proc/self/cmdline");
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[256];
            int r;
            while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
            String s = new String(bos.toByteArray(), StandardCharsets.UTF_8);
            int z = s.indexOf('\0');
            return z > 0 ? s.substring(0, z) : s.trim();
        } catch (Throwable t) {
            return "(unknown)";
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }
}
