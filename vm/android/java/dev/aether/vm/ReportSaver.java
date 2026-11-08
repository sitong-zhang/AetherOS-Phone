package dev.aether.vm;

import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.widget.Toast;

import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Aether VM 完整报告落盘器（自研，1.0.7）。
 *
 * 【为什么存在】此前旧版按钮把报告塞进剪贴板，用户粘贴到聊天框时会被
 * 长度限制截断 —— 六版以来开发者反复拿不到 QEMU 报错 / native backtrace。
 * 本类把【完整、不做任何粘贴长度裁剪】的报告直接写成 .txt 文件，并在点击
 * 后自动弹出系统分享面板，用户把原文件发给开发者即可，永远不会被截断。
 *
 * 双落点：
 *  A) app 私有 files/reports/ —— 零权限、一定成功；经 ReportProvider 生成
 *     content:// URI 分享，兼容所有 Android 版本（含 minSdk 21）。
 *  B) 公共「下载/AetherVM/」—— API 29+ 走 MediaStore，零权限；用户也能在
 *     文件管理器里直接看到，分享优先用它。
 * 两处都失败才算失败；任一成功即可分享原始文件。
 */
public final class ReportSaver {

    private static final String SUBDIR = "AetherVM";
    private static final String MIME = "text/plain";

    private ReportSaver() {}

    public static final class Result {
        public final File file;         // 私有落点（可能为 null）
        public final Uri shareUri;      // 可分享的 content://（可能为 null）
        public final String where;      // 人类可读位置
        public final int bytes;         // 实际写入字节数
        Result(File f, Uri u, String w, int n) {
            file = f; shareUri = u; where = w; bytes = n;
        }
        public boolean ok() { return file != null || shareUri != null; }
    }

    /** 把完整报告写入文件，返回可分享 URI；不弹 UI，供崩溃瞬间自动落盘用。 */
    public static Result save(Context c, String report) {
        byte[] data = (report == null ? "(空报告)" : report)
                .getBytes(StandardCharsets.UTF_8);
        String ts = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.CHINA)
                .format(new Date());
        String name = "AetherVM-report-" + ts + ".txt";

        // ---- 落点 A：私有目录，零权限、必成 ----
        File priv = null;
        FileOutputStream fos = null;
        try {
            File dir = new File(c.getFilesDir(), "reports");
            if (!dir.exists() && !dir.mkdirs()) { /* 继续尝试 */ }
            priv = new File(dir, name);
            fos = new FileOutputStream(priv, false);
            fos.write(data);
            fos.flush();
        } catch (Throwable ignored) {
            priv = null;
        } finally {
            close(fos);
        }

        // ---- 落点 B：公共下载目录（API 29+，零权限）----
        Uri pub = null;
        String pubPath = null;
        if (Build.VERSION.SDK_INT >= 29) {
            OutputStream os = null;
            try {
                ContentValues cv = new ContentValues();
                // 这些是 static final String，编译期内联，低版本无运行时类依赖
                cv.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
                cv.put(MediaStore.MediaColumns.MIME_TYPE, MIME);
                cv.put(MediaStore.MediaColumns.RELATIVE_PATH,
                        Environment.DIRECTORY_DOWNLOADS + "/" + SUBDIR);
                cv.put(MediaStore.MediaColumns.IS_PENDING, 1);
                // 用标准 URI 字符串，避免引用 API 29 的 MediaStore.Downloads 类
                Uri u = c.getContentResolver().insert(
                        Uri.parse("content://media/external/downloads"), cv);
                if (u != null) {
                    os = c.getContentResolver().openOutputStream(u);
                    if (os != null) { os.write(data); os.flush(); }
                    cv.clear();
                    cv.put(MediaStore.MediaColumns.IS_PENDING, 0);
                    c.getContentResolver().update(u, cv, null, null);
                    pub = u;
                    pubPath = Environment.DIRECTORY_DOWNLOADS + "/" + SUBDIR + "/" + name;
                }
            } catch (Throwable ignored) {
                pub = null;
            } finally {
                close(os);
            }
        }

        // ---- 分享 URI：公共优先，私有 Provider 兜底 ----
        Uri share = pub;
        if (share == null && priv != null) {
            share = new Uri.Builder()
                    .scheme("content")
                    .authority(c.getPackageName() + ".reports")
                    .appendPath("reports")
                    .appendPath(name)
                    .build();
        }
        String where = pubPath != null ? pubPath
                : (priv != null ? priv.getAbsolutePath() : "(未能写入文件)");
        return new Result(priv, share, where, data.length);
    }

    /** 界面按钮入口：保存 + Toast 位置 + 自动弹分享面板，一步把原文件发出去。 */
    public static void saveAndShare(Context c, String report) {
        Result r = save(c, report);
        if (!r.ok()) {
            Toast.makeText(c, "保存失败：无法写入文件（请检查存储）", Toast.LENGTH_LONG).show();
            return;
        }
        Toast.makeText(c,
                "完整报告已保存：" + r.bytes + " 字节（未截断）\n" + r.where
                        + "\n已打开发送面板，把该 .txt 文件发给开发者即可",
                Toast.LENGTH_LONG).show();
        if (r.shareUri == null) return;
        try {
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType(MIME);
            i.putExtra(Intent.EXTRA_STREAM, r.shareUri);
            i.putExtra(Intent.EXTRA_SUBJECT, "Aether VM 完整诊断报告 .txt");
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent chooser = Intent.createChooser(i, "把完整报告文件发给开发者");
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.startActivity(chooser);
        } catch (Throwable t) {
            Toast.makeText(c, "发送面板未打开，文件已存于：" + r.where,
                    Toast.LENGTH_LONG).show();
        }
    }

    private static void close(Closeable c) {
        if (c != null) try { c.close(); } catch (Throwable ignored) {}
    }
}
