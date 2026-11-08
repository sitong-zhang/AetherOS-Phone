package dev.aether.vm;

import android.os.SystemClock;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * 启动阶段埋点（自研）。
 * 每走一步就实时追加写文件，即使主进程当场崩掉，下次冷启动也能读到
 * 「崩之前走到了哪一步」。
 *
 * 【1.0.6】1.0.5 不裁剪历史，报告里堆了十几段历史启动，把关键日志挤到尾部。
 * 【1.0.7】报告改为整份落盘成 .txt（不再粘贴），这里放宽总长度上限；仍只保留
 * 「上一次 + 本次」两段，避免文件无限增长，同时保证诊断浮层刷新不卡。
 */
public final class BootTrace {
    private static final Object LOCK = new Object();
    private static final String SEP = "===== 新一次启动";
    private static final int MAX_CHARS = 30000;  // 整份日志上限（字符），防无限增长
    private static File file;
    private static long t0;

    private BootTrace() {}

    public static void init(File crashDir) {
        synchronized (LOCK) {
            try {
                if (!crashDir.exists()) crashDir.mkdirs();
                file = new File(crashDir, "boot_trace.txt");
                t0 = SystemClock.elapsedRealtime();
                trimHistory();
                mark("===== 新一次启动 "
                        + new SimpleDateFormat("MM-dd HH:mm:ss", Locale.CHINA).format(new Date())
                        + " =====");
            } catch (Throwable ignored) {
            }
        }
    }

    /** 只保留最后一段「已有历史」+ 即将追加的本次；再按长度上限兜底裁尾 */
    private static void trimHistory() {
        if (file == null || !file.exists()) return;
        FileOutputStream out = null;
        try {
            String all = readRaw();
            int last = all.lastIndexOf(SEP);
            int prev = last > 0 ? all.lastIndexOf(SEP, last - 1) : -1;
            int from = prev >= 0 ? prev : (last >= 0 ? last : 0);
            String keep = all.substring(from);
            if (keep.length() > MAX_CHARS)
                keep = "…(更早历史略)\n" + keep.substring(keep.length() - MAX_CHARS);
            out = new FileOutputStream(file, false);       // 关键：覆盖而非追加
            out.write(keep.getBytes(StandardCharsets.UTF_8));
            out.flush();
        } catch (Throwable ignored) {
        } finally {
            if (out != null) try { out.close(); } catch (Throwable ignored) {}
        }
    }

    public static void mark(String stage) {
        synchronized (LOCK) {
            if (file == null) return;   // :crash 进程未初始化，静默跳过
            FileOutputStream out = null;
            try {
                out = new FileOutputStream(file, true);
                String line = "[+" + (SystemClock.elapsedRealtime() - t0) + "ms] " + stage + "\n";
                out.write(line.getBytes(StandardCharsets.UTF_8));
                out.flush();
            } catch (Throwable ignored) {
            } finally {
                if (out != null) try { out.close(); } catch (Throwable ignored) {}
            }
        }
    }

    public static String read() {
        synchronized (LOCK) {
            return readRaw();
        }
    }

    /** 调用方需已持有 LOCK；原样读全文 */
    private static String readRaw() {
        if (file == null || !file.exists()) return "(无启动进度记录)";
        FileInputStream in = null;
        try {
            in = new FileInputStream(file);
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int r;
            while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
            return new String(bos.toByteArray(), StandardCharsets.UTF_8);
        } catch (Throwable t) {
            return "(读取启动进度失败: " + t + ")";
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }
}
