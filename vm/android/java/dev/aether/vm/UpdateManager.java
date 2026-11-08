package dev.aether.vm;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;

/**
 * 应用内更新器：读取公开 GitHub Releases 的最新版本，下载并校验新 APK，
 * 再交给系统安装器完成升级。
 *
 * 该模块只访问公开 API 与公开下载直链，不携带任何访问凭证；公开 Release
 * 的读取不需要认证。版本号取自 Release 的 tag_name（形如 v1.0.16），按
 * 数值分段与本机 versionName 比较。下载完成后按 Release 资产提供的
 * sha256 digest 逐字节校验，校验不通过不触发安装。
 *
 * 所有网络与文件读写均在工作线程执行，结果通过 UiBridge 回传界面线程。
 */
public final class UpdateManager {

    public static final String REPO = "sitong-zhang/AetherOS-Phone";
    public static final String API_LATEST =
            "https://api.github.com/repos/" + REPO + "/releases/latest";
    public static final String DIRECT_APK =
            "https://github.com/" + REPO + "/releases/latest/download/AetherVM.apk";
    public static final String APK_FILE = "AetherVM-update.apk";
    public static final String UPDATE_AUTHORITY = "dev.aether.vm.update";

    private static final int CONNECT_TIMEOUT_MS = 15000;
    private static final int READ_TIMEOUT_MS = 30000;

    private UpdateManager() {}

    /** 最新版本信息。 */
    public static final class Info {
        public String tag = "";
        public String name = "";
        public String body = "";
        public String apkUrl = "";
        public String digest = "";
        public long size = 0;
        public boolean newer = false;
        public String localVersion = "";
    }

    public interface Callback {
        void onCheckStart();
        void onCheckDone(Info info, String error);
        void onProgress(int percent, long done, long total);
        void onDownloadDone(File apk, String error);
    }

    /** 把结果切回界面线程；Activity 销毁后回调被丢弃，避免持有已失效上下文。 */
    public interface UiBridge {
        void post(Runnable r);
        boolean isAlive();
    }

    private static void fire(UiBridge ui, Callback cb, Runnable r) {
        if (ui == null || !ui.isAlive()) return;
        ui.post(r);
    }

    public static String localVersion(Context ctx) {
        try {
            PackageInfo pi = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            return pi.versionName == null ? "" : pi.versionName;
        } catch (Throwable t) {
            return "";
        }
    }

    /** 数值分段比较版本字符串，忽略前导 v 与非数字后缀。 */
    static int compareVersion(String a, String b) {
        int[] va = parseVersion(a), vb = parseVersion(b);
        for (int i = 0; i < 3; i++) {
            if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
        }
        return 0;
    }

    private static int[] parseVersion(String s) {
        int[] out = new int[3];
        if (s == null) return out;
        String t = s.trim();
        if (t.startsWith("v") || t.startsWith("V")) t = t.substring(1);
        String[] parts = t.split("[.\\-]");
        for (int i = 0; i < Math.min(3, parts.length); i++) {
            StringBuilder digits = new StringBuilder();
            for (int k = 0; k < parts[i].length() && digits.length() < 9; k++) {
                char c = parts[i].charAt(k);
                if (c >= '0' && c <= '9') digits.append(c);
                else break;
            }
            try { out[i] = Integer.parseInt(digits.toString()); }
            catch (NumberFormatException ignored) { out[i] = 0; }
        }
        return out;
    }

    /** 异步检查最新版本。 */
    public static void check(final Context ctx, final UiBridge ui, final Callback cb) {
        fire(ui, cb, new Runnable() {
            @Override public void run() { cb.onCheckStart(); }
        });
        new Thread(new Runnable() {
            @Override public void run() {
                Info info = null;
                String error = null;
                try {
                    info = fetchLatest(ctx);
                } catch (Throwable t) {
                    error = describe(t);
                }
                final Info fi = info;
                final String fe = error;
                fire(ui, cb, new Runnable() {
                    @Override public void run() { cb.onCheckDone(fi, fe); }
                });
            }
        }, "aether-update-check").start();
    }

    private static Info fetchLatest(Context ctx) throws Exception {
        HttpURLConnection conn = open(API_LATEST);
        try {
            int code = conn.getResponseCode();
            if (code != 200) throw new IllegalStateException("Release 接口返回 " + code);
            String json = readAll(conn.getInputStream(), 512 * 1024);
            JSONObject root = new JSONObject(json);
            Info in = new Info();
            in.tag = root.optString("tag_name", "");
            in.name = root.optString("name", "");
            in.body = root.optString("body", "");
            in.localVersion = localVersion(ctx);
            in.newer = compareVersion(in.tag, in.localVersion) > 0;

            JSONArray assets = root.optJSONArray("assets");
            if (assets != null) {
                for (int i = 0; i < assets.length(); i++) {
                    JSONObject a = assets.getJSONObject(i);
                    if (!"AetherVM.apk".equals(a.optString("name"))) continue;
                    in.apkUrl = a.optString("browser_download_url", "");
                    in.size = a.optLong("size", 0);
                    in.digest = a.optString("digest", "");
                    break;
                }
            }
            if (in.apkUrl.isEmpty()) in.apkUrl = DIRECT_APK;
            return in;
        } finally {
            conn.disconnect();
        }
    }

    /** 异步下载并校验 APK。 */
    public static void download(final Context ctx, final Info info,
                                final UiBridge ui, final Callback cb) {
        new Thread(new Runnable() {
            @Override public void run() {
                File apk = null;
                String error = null;
                try {
                    apk = downloadAndVerify(ctx, info, ui, cb);
                } catch (Throwable t) {
                    error = describe(t);
                }
                final File fa = apk;
                final String fe = error;
                fire(ui, cb, new Runnable() {
                    @Override public void run() { cb.onDownloadDone(fa, fe); }
                });
            }
        }, "aether-update-download").start();
    }

    private static File downloadAndVerify(Context ctx, Info info,
                                          UiBridge ui, Callback cb) throws Exception {
        File dir = updateDir(ctx);
        if (!dir.exists() && !dir.mkdirs())
            throw new IllegalStateException("无法创建更新目录");
        File apk = new File(dir, APK_FILE);
        if (apk.exists() && !apk.delete())
            throw new IllegalStateException("无法清理旧安装包");

        HttpURLConnection conn = open(info.apkUrl.isEmpty() ? DIRECT_APK : info.apkUrl);
        try {
            int code = conn.getResponseCode();
            if (code != 200 && code != 302 && code != 301)
                throw new IllegalStateException("下载返回 " + code);
            long total = info.size;
            String cl = conn.getHeaderField("Content-Length");
            if (cl != null) {
                try { total = Long.parseLong(cl.trim()); }
                catch (NumberFormatException ignored) { total = info.size; }
            }
            try (InputStream in = conn.getInputStream();
                 OutputStream out = new FileOutputStream(apk)) {
                byte[] buf = new byte[64 * 1024];
                long done = 0;
                int r;
                while ((r = in.read(buf)) > 0) {
                    out.write(buf, 0, r);
                    done += r;
                    final int pct = total > 0 ? (int) (done * 100 / total) : -1;
                    final long fdone = done, ftotal = total;
                    fire(ui, cb, new Runnable() {
                        @Override public void run() { cb.onProgress(pct, fdone, ftotal); }
                    });
                }
                out.flush();
            }
        } finally {
            conn.disconnect();
        }

        if (apk.length() < 1024 * 1024)
            throw new IllegalStateException("安装包体积异常：" + apk.length() + " B");
        if (info.size > 0 && apk.length() != info.size)
            throw new IllegalStateException("体积不符：期望 " + info.size + " B，实际 " + apk.length() + " B");
        if (info.digest.startsWith("sha256:")) {
            String want = info.digest.substring(7).toLowerCase();
            String got = sha256(apk);
            if (!want.equals(got))
                throw new IllegalStateException("SHA-256 校验失败");
        }
        return apk;
    }

    /** 用自研 Provider 暴露 content:// URI，拉起系统安装器。 */
    public static void install(Context ctx, File apk) {
        Uri uri = Uri.parse("content://" + UPDATE_AUTHORITY + "/" + APK_FILE);
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(uri, "application/vnd.android.package-archive");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ctx.startActivity(i);
    }

    public static File updateDir(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        File base = ext != null ? ext : ctx.getFilesDir();
        return new File(base, "updates");
    }

    private static HttpURLConnection open(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestMethod("GET");
        c.setConnectTimeout(CONNECT_TIMEOUT_MS);
        c.setReadTimeout(READ_TIMEOUT_MS);
        c.setInstanceFollowRedirects(true);
        c.setRequestProperty("User-Agent", "AetherVM-Updater/1.0");
        c.setRequestProperty("Accept", "application/vnd.github+json");
        return c;
    }

    private static String readAll(InputStream in, int limit) throws Exception {
        StringBuilder sb = new StringBuilder();
        byte[] buf = new byte[4096];
        int r, n = 0;
        while ((r = in.read(buf)) > 0) {
            n += r;
            if (n > limit) break;
            sb.append(new String(buf, 0, r, "UTF-8"));
        }
        return sb.toString();
    }

    private static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] buf = new byte[64 * 1024];
            int r;
            while ((r = in.read(buf)) > 0) md.update(buf, 0, r);
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest()) sb.append(String.format("%02x", b & 0xFF));
        return sb.toString();
    }

    private static String describe(Throwable t) {
        String m = t.getMessage();
        if (m == null || m.isEmpty()) m = t.getClass().getSimpleName();
        return m;
    }
}
