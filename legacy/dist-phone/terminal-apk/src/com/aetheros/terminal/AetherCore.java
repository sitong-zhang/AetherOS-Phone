package com.aetheros.terminal;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.*;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.GZIPInputStream;

/**
 * AetherOS 自包含终端 App 的核心逻辑：
 *  - 系统镜像下载（多镜像兜底）
 *  - tar.gz 解包（纯 JDK）
 *  - QEMU 进程管理（nativeLibraryDir 内 exec）
 *  - 状态机（installing/installed/booted/version）
 *
 * 线程安全：所有状态字段用 synchronized 保护。
 */
public class AetherCore {
    private static final String TAG = "AetherCore";

    public static final String REPO = "sitong-zhang/AetherOS-Phone";
    public static final String TAG_RELEASE = "phone-v18";
    public static final String SYSTEM_TARBALL = "aetheros-phone.tar.gz";
    public static final String KERNEL_NAME = "aetheros-kernel-aarch64.Image";
    public static final String ROOTFS_NAME = "aetheros-rootfs.ext4";

    public static final String[] MIRRORS = {
            "https://gh-proxy.com/https://github.com",
            "https://ghfast.top/https://github.com",
            "https://github.com",
    };

    private final Context ctx;
    private final File workDir;      // filesDir/aetheros-app
    private final File systemDir;    // workDir/system
    private final File logFile;      // workDir/aetheros-app.log
    private final File serialLog;    // workDir/serial.log

    private final Object lock = new Object();
    private boolean installing = false;
    private boolean installed = false;
    private String version = TAG_RELEASE;
    private Process qemuProcess = null;
    private int qemuPid = -1;
    private String vncHost = "127.0.0.1";
    private int vncPort = 5900;
    private String lastError = null;
    /** 最近一次 QEMU 退出信息（code + 关键错误行），供状态页直接展示。 */
    private String qemuExitInfo = null;
    /* 内嵌 HTTP 服务端口：经内核命令行 aether_port= 注入客机，客机内
     * 「系统更新」应用经 10.0.2.2:<port> 访问宿主（#294）。 */
    private int hostPort = 0;

    private final List<String> logLines = java.util.Collections.synchronizedList(new ArrayList<String>());

    public AetherCore(Context ctx) {
        this.ctx = ctx;
        this.workDir = new File(ctx.getFilesDir(), "aetheros-app");
        this.systemDir = new File(workDir, "system");
        this.logFile = new File(workDir, "aetheros-app.log");
        this.serialLog = new File(workDir, "serial.log");
        if (!workDir.exists()) workDir.mkdirs();
        if (!systemDir.exists()) systemDir.mkdirs();
        startVncWatchdog();
        installed = systemPresent();
        if (installed) {
            log("检测到已有系统镜像，跳过下载（删除 system 目录可重装）");
            String v = readImageVersion();
            if (v != null) {
                synchronized (lock) { version = v; }
                log("镜像自报版本: " + v);
            }
        }
    }

    /** 读解包后镜像里的 /etc/aether-version（构建期写入的 release tag）。 */
    private String readImageVersion() {
        File f = new File(systemDir, "etc/aether-version");
        if (!f.isFile()) return null;
        try {
            byte[] buf = new byte[64];
            try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
                int n = in.read(buf);
                if (n <= 0) return null;
                String s = new String(buf, 0, n, "UTF-8").trim();
                return s.isEmpty() ? null : s;
            }
        } catch (IOException e) {
            return null;
        }
    }

    // ------------------------------------------------------------------
    // 日志
    // ------------------------------------------------------------------
    public void log(String msg) {
        String line = "[aether] " + msg;
        synchronized (logLines) {
            logLines.add(line);
            if (logLines.size() > 500) logLines.remove(0);
        }
        Log.i(TAG, msg);
        try (FileWriter fw = new FileWriter(logFile, true)) {
            fw.write(line + "\n");
        } catch (IOException ignored) {}
    }

    public List<String> tailLog(int n) {
        synchronized (logLines) {
            int from = Math.max(0, logLines.size() - n);
            return new ArrayList<>(logLines.subList(from, logLines.size()));
        }
    }

    /** 全量日志快照（SSE 增量推送用：内部同步拷贝，环形缓冲截断时调用方靠 size 变小感知）。 */
    public List<String> allLog() {
        synchronized (logLines) {
            return new ArrayList<>(logLines);
        }
    }

    public String serialTail(int n) {
        File f = serialLog;
        if (!f.isFile() || f.length() == 0) return "";
        try {
            byte[] buf = new byte[(int) Math.min(f.length(), 64 * 1024)];
            try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
                raf.seek(Math.max(0, f.length() - buf.length));
                raf.readFully(buf);
            }
            String s = new String(buf, "UTF-8");
            // 去掉首行可能被截断的半行
            int nl = s.indexOf('\n');
            if (nl > 0 && nl < s.length() - 1) s = s.substring(nl + 1);
            return s;
        } catch (IOException e) {
            return "";
        }
    }

    // ------------------------------------------------------------------
    // 状态
    // ------------------------------------------------------------------
    public boolean systemPresent() {
        return new File(systemDir, KERNEL_NAME).isFile() && new File(systemDir, ROOTFS_NAME).isFile();
    }

    public boolean isInstalling() { synchronized (lock) { return installing; } }

    public boolean isInstalled() {
        synchronized (lock) { return installed || systemPresent(); }
    }

    /** QEMU 进程是否还活着（不含端口探测）。 */
    public boolean isQemuRunning() {
        synchronized (lock) {
            if (qemuProcess == null) return false;
            try {
                qemuProcess.exitValue();
                qemuProcess = null;   // 已退出
                return false;
            } catch (IllegalThreadStateException e) {
                return true;
            }
        }
    }

    /**
     * 是否已能出画面：进程活着 **且** VNC 端口在监听。
     *
     * 旧实现只要进程还在就当 booted。QEMU 被 linker 拒绝时进程能存在几百毫秒，
     * 这一瞬间被前端 poll 到就把 lastBooted 锁死，于是界面卡在“连 5900”死循环。
     */
    public boolean isBooted() {
        return isQemuRunning() && vncAlive();
    }

    /** 已拉起但画面还没就绪（内核启动期）。 */
    public boolean isStarting() {
        synchronized (lock) {
            if (qemuProcess == null) return false;
        }
        return !vncAlive();
    }

    public String getVersion() { synchronized (lock) { return version; } }
    public String getVncHost() { synchronized (lock) { return vncHost; } }
    public int getVncPort() { synchronized (lock) { return vncPort; } }
    public String getLastError() { synchronized (lock) { return lastError; } }

    /** 设置内嵌 HTTP 服务端口（MainActivity 探测成功后调用；客机经它访问宿主）。 */
    public void setHostPort(int p) { synchronized (lock) { hostPort = p; } }
    public int getHostPort() { synchronized (lock) { return hostPort; } }

    /**
     * VNC 端口三态探测（phone-v18）：
     *   DEAD    —— 连接被拒：loopback 上拒连只可能是端口没在监听（QEMU 真没了）
     *   UNKNOWN —— 超时/重置：QEMU 主循环忙着推 4.6MB 首帧时，accept 队列
     *              排不进我们每 1.5 秒的探测连接。这**不是**掉线！
     *              v17 及以前把它当掉线，pollStatus 立刻 closeVNC，浏览器
     *              接收缓冲里还压着半帧未读数据 → 内核 RST → 服务端看到
     *              "Connection reset / Broken pipe" → 用户看到"一直重连"。
     *   ALIVE   —— 能连上
     */
    private static final int VNC_DEAD = 0, VNC_UNKNOWN = 1, VNC_ALIVE = 2;

    private int vncProbe() {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress(vncHost, vncPort), 1500);
            return VNC_ALIVE;
        } catch (java.net.ConnectException e) {
            return VNC_DEAD;
        } catch (IOException e) {
            return VNC_UNKNOWN;
        }
    }

    /** 仅当"确定没在监听"才算掉线；探测超时按活着处理（配合 20s 健康探针兜底）。 */
    private boolean vncAlive() {
        return vncProbe() != VNC_DEAD;
    }

    // ------------------------------------------------------------------
    // 下载（多镜像兜底，流式写盘避免 OOM）
    // ------------------------------------------------------------------
    private boolean download(String url, File dest, int timeoutSec, String[] finalUrlOut) throws IOException {
        String gh = "https://github.com";
        IOException last = null;
        for (String base : MIRRORS) {
            String b = base.endsWith("/") ? base.substring(0, base.length() - 1) : base;
            int gi = url.indexOf(gh);
            String full = gi >= 0 ? url.substring(0, gi) + b + url.substring(gi + gh.length()) : url;
            HttpURLConnection conn = null;
            try {
                log("下载通道: " + abbreviate(full));
                URL u = new URL(full);
                conn = (HttpURLConnection) u.openConnection();
                conn.setInstanceFollowRedirects(true);
                conn.setConnectTimeout(15000);
                conn.setReadTimeout(timeoutSec * 1000);
                conn.setRequestProperty("User-Agent", "AetherOS-Phone-App");
                int code = conn.getResponseCode();
                if (code != 200) throw new IOException("HTTP " + code);
                // 跟随重定向后的最终 URL：/releases/download/<tag>/... 里的 tag
                // 就是真实版本号（latest 通道安装后 version 据此自报，避免
                // 与 checkUpdate 的 latest tag 永远不等而造成假阳性更新）
                if (finalUrlOut != null)
                    finalUrlOut[0] = conn.getURL().toString();
                long len = conn.getContentLengthLong();
                File tmp = new File(dest.getAbsolutePath() + ".part");
                try (InputStream in = conn.getInputStream();
                     OutputStream out = new FileOutputStream(tmp)) {
                    byte[] buf = new byte[65536];
                    long total = 0;
                    int n;
                    long lastPct = -1;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        total += n;
                        if (len > 0) {
                            long pct = total * 100 / len;
                            if (pct >= lastPct + 10) {
                                lastPct = pct;
                                log(String.format("下载进度 %d%% (%d/%d)", pct, total, len));
                            }
                        }
                    }
                    out.flush();
                }
                if (tmp.length() < 1024 * 1024) {
                    tmp.delete();
                    throw new IOException("文件过小(" + tmp.length() + "B)，疑似错误页");
                }
                if (dest.exists()) dest.delete();
                if (!tmp.renameTo(dest)) throw new IOException("重命名失败");
                log("下载完成: " + dest.getName() + " (" + dest.length() + " 字节)");
                return true;
            } catch (IOException e) {
                last = e;
                log("通道失败: " + e.getMessage());
            } finally {
                if (conn != null) conn.disconnect();
            }
        }
        throw new IOException("所有镜像均不可用: " + (last == null ? "?" : last.getMessage()));
    }

    private static String abbreviate(String s) {
        return s.length() > 90 ? s.substring(0, 90) + "…" : s;
    }

    /** 从最终下载 URL 提取 release tag（/releases/download/<tag>/...）。 */
    private static String tagFromUrl(String u, String fallback) {
        if (u == null) return fallback;
        String marker = "/releases/download/";
        int i = u.indexOf(marker);
        if (i < 0) return fallback;
        String rest = u.substring(i + marker.length());
        int slash = rest.indexOf('/');
        String tag = slash > 0 ? rest.substring(0, slash) : rest;
        return tag.isEmpty() ? fallback : tag;
    }

    // ------------------------------------------------------------------
    // tar.gz 解包（纯 JDK，仅提取普通文件与目录，拒绝路径穿越）
    // ------------------------------------------------------------------
    private void extractTarGz(File tgz, File destDir) throws IOException {
        log("解包系统镜像 …");
        int count = 0;
        try (InputStream fin = new FileInputStream(tgz);
             GZIPInputStream gz = new GZIPInputStream(fin, 65536);
             BufferedInputStream in = new BufferedInputStream(gz, 65536)) {
            byte[] hdr = new byte[512];
            while (true) {
                if (!readFull(in, hdr)) break;
                if (isZero(hdr)) break; // 结束块
                String name = field(hdr, 0, 100);
                long size = parseOctal(field(hdr, 124, 12));
                char type = (char) hdr[156];
                String linkname = field(hdr, 157, 100);
                String prefix = field(hdr, 345, 155);
                if (!prefix.isEmpty()) name = prefix + "/" + name;
                // 跳过 pax/gnu 扩展头
                if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
                    skip(in, size);
                    continue;
                }
                if (name.startsWith("./")) name = name.substring(2);
                if (name.isEmpty()) { skip(in, size); continue; }
                File out = new File(destDir, name);
                String canon = out.getCanonicalPath();
                if (!canon.startsWith(destDir.getCanonicalPath() + File.separator)
                        && !canon.equals(destDir.getCanonicalPath())) {
                    throw new IOException("拒绝路径穿越: " + name);
                }
                if (type == '5') { // 目录
                    if (!out.exists()) out.mkdirs();
                    skip(in, size);
                } else if (type == '0' || type == '\0') { // 普通文件
                    File parent = out.getParentFile();
                    if (parent != null && !parent.exists()) parent.mkdirs();
                    try (OutputStream os = new FileOutputStream(out)) {
                        long remain = size;
                        byte[] buf = new byte[65536];
                        while (remain > 0) {
                            int want = (int) Math.min(buf.length, remain);
                            int n = in.read(buf, 0, want);
                            if (n < 0) break;
                            os.write(buf, 0, n);
                            remain -= n;
                        }
                    }
                    skipPad(in, size);
                    count++;
                } else if (type == '1' || type == '2') { // 硬/软链接：按普通文件处理（内容跟随）
                    // rootfs 镜像不需要链接，直接跳过
                    skip(in, size);
                } else {
                    skip(in, size);
                }
            }
        }
        log("解包完成: " + count + " 个文件");
    }

    private static boolean readFull(InputStream in, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int n = in.read(b, off, b.length - off);
            if (n < 0) return off == 0 ? false : true;
            off += n;
        }
        return true;
    }

    private static boolean isZero(byte[] b) {
        for (byte x : b) if (x != 0) return false;
        return true;
    }

    private static String field(byte[] hdr, int off, int len) {
        int end = off;
        while (end < off + len && hdr[end] != 0) end++;
        return new String(hdr, off, end - off).trim();
    }

    private static long parseOctal(String s) {
        s = s.trim();
        if (s.isEmpty()) return 0;
        try { return Long.parseLong(s, 8); } catch (NumberFormatException e) { return 0; }
    }

    private static void skip(InputStream in, long n) throws IOException {
        long remain = n;
        while (remain > 0) {
            long skipped = in.skip(remain);
            if (skipped <= 0) {
                if (in.read() < 0) break;
                skipped = 1;
            }
            remain -= skipped;
        }
        long pad = (512 - (n % 512)) % 512;
        skipExact(in, pad);
    }

    private static void skipPad(InputStream in, long n) throws IOException {
        long pad = (512 - (n % 512)) % 512;
        skipExact(in, pad);
    }

    private static void skipExact(InputStream in, long n) throws IOException {
        for (long i = 0; i < n; i++) {
            if (in.read() < 0) break;
        }
    }

    // ------------------------------------------------------------------
    // 安装（后台线程）
    // ------------------------------------------------------------------
    public boolean startInstall() {
        synchronized (lock) {
            if (installing) return false;
            if (isInstalled() && isBooted()) return false;
            installing = true;
        }
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() { installThread(); }
        }, "aether-install");
        t.setDaemon(true);
        t.start();
        return true;
    }

    private void installThread() {
        try {
            if (!systemPresent()) {
                File tb = new File(workDir, SYSTEM_TARBALL);
                log("开始下载系统镜像（优先最新版）…");
                String urlLatest = "https://github.com/" + REPO + "/releases/latest/download/" + SYSTEM_TARBALL;
                String urlTag = "https://github.com/" + REPO + "/releases/download/" + TAG_RELEASE + "/" + SYSTEM_TARBALL;
                boolean ok = false;
                try {
                    String[] finalUrl = new String[1];
                    download(urlLatest, tb, 300, finalUrl);
                    synchronized (lock) { version = tagFromUrl(finalUrl[0], "latest"); }
                    log("已安装版本: " + getVersion());
                    ok = true;
                } catch (Exception e) {
                    log("latest 通道失败，兜底 " + TAG_RELEASE + " …");
                    try {
                        download(urlTag, tb, 300, null);
                        ok = true;
                    } catch (Exception e2) {
                        synchronized (lock) { lastError = e2.getMessage(); }
                        log("下载失败: " + e2.getMessage());
                        return;
                    }
                }
                if (!ok) return;
                extractTarGz(tb, systemDir);
                tb.delete();
                synchronized (lock) { installed = true; }
                // 镜像自报版本优先（URL 猜 tag 在镜像站重定向下会丢）
                String imgVer = readImageVersion();
                if (imgVer != null) {
                    synchronized (lock) { version = imgVer; }
                    log("镜像自报版本: " + imgVer);
                }
            } else {
                log("系统已存在，跳过下载");
                synchronized (lock) { installed = true; }
                String v = readImageVersion();
                if (v != null) {
                    synchronized (lock) { version = v; }
                    log("镜像自报版本: " + v);
                }
            }
            launchQemu();
        } catch (Exception e) {
            log("安装异常: " + e);
            synchronized (lock) { lastError = String.valueOf(e); }
        } finally {
            synchronized (lock) { installing = false; }
        }
    }

    // ------------------------------------------------------------------
    // QEMU 启动 / 停止
    // ------------------------------------------------------------------
    /**
     * QEMU 运行期需要的静态资源（keymaps 等）。APK assets 里带一份，
     * 首次启动解到 files/aetheros-app/qemu-data，供 -L 指向。
     */
    private File prepareQemuData() {
        File dst = new File(workDir, "qemu-data");
        if (!dst.exists()) dst.mkdirs();
        try {
            copyAssetTree("qemu-data", dst);
        } catch (IOException e) {
            log("准备 QEMU 资源失败: " + e.getMessage());
        }
        return dst;
    }

    private void copyAssetTree(String assetPath, File dst) throws IOException {
        String[] kids = ctx.getAssets().list(assetPath);
        if (kids != null && kids.length > 0) {
            if (!dst.exists()) dst.mkdirs();
            for (String k : kids) copyAssetTree(assetPath + "/" + k, new File(dst, k));
            return;
        }
        if (dst.isFile() && dst.length() > 0) return;   // 已解过
        File parent = dst.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();
        try (InputStream in = ctx.getAssets().open(assetPath);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    /** 在 [from,to] 里找一个本机空闲 TCP 端口；全占用则返回 from。 */
    private int pickFreePort(int from, int to) {
        for (int p = from; p <= to; p++) {
            java.net.ServerSocket ss = null;
            try {
                ss = new java.net.ServerSocket();
                ss.setReuseAddress(true);
                ss.bind(new InetSocketAddress("127.0.0.1", p));
                return p;
            } catch (IOException ignored) {
                // 被占用，试下一个
            } finally {
                if (ss != null) try { ss.close(); } catch (IOException ignored) {}
            }
        }
        return from;
    }

    /** 读文件末尾若干行（用于把 QEMU 的报错显示到 App 日志里）。 */
    private List<String> tailFile(File f, int lines) {
        List<String> out = new ArrayList<>();
        if (!f.isFile() || f.length() == 0) return out;
        try {
            int cap = (int) Math.min(f.length(), 32 * 1024);
            byte[] buf = new byte[cap];
            try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
                raf.seek(f.length() - cap);
                raf.readFully(buf);
            }
            String[] all = new String(buf, "UTF-8").split("\n");
            for (String l : all) {
                String t = l.trim();
                if (!t.isEmpty()) out.add(t);
            }
            if (out.size() > lines) out = new ArrayList<>(out.subList(out.size() - lines, out.size()));
        } catch (IOException ignored) {}
        return out;
    }

    private File qemuBinary() {
        String libDir = ctx.getApplicationInfo().nativeLibraryDir;
        return new File(libDir, "libqemu-system-aarch64.so");
    }

    public boolean launchQemu() {
        if (isBooted()) {
            log("系统已在运行");
            return true;
        }
        if (!systemPresent()) {
            log("系统未安装，请先执行安装");
            return false;
        }
        File qemu = qemuBinary();
        if (!qemu.canExecute()) {
            // installd 提取时通常已带执行位；兜底尝试 chmod（自家 app_data_file 允许）
            try {
                Process ch = new ProcessBuilder("chmod", "750", qemu.getAbsolutePath())
                        .redirectErrorStream(true).start();
                ch.waitFor();
            } catch (Exception ignored) {}
        }
        String libDir = qemu.getParentFile().getAbsolutePath();
        File kernel = new File(systemDir, KERNEL_NAME);
        File rootfs = new File(systemDir, ROOTFS_NAME);

        File qdata = prepareQemuData();

        List<String> cmd = new ArrayList<>();
        cmd.add(qemu.getAbsolutePath());
        // QEMU 编译期前缀是 Termux 的 /data/data/com.termux/files/usr，
        // 该路径在 App 里不存在；不指 -L 就找不到 keymaps（键盘输入全废）。
        cmd.add("-L"); cmd.add(qdata.getAbsolutePath());
        cmd.add("-M"); cmd.add("virt");
        cmd.add("-cpu"); cmd.add("cortex-a57");
        cmd.add("-smp"); cmd.add("2");
        cmd.add("-m"); cmd.add("512M");
        cmd.add("-kernel"); cmd.add(kernel.getAbsolutePath());
        cmd.add("-drive"); cmd.add("if=virtio,format=raw,file=" + rootfs.getAbsolutePath());
        // aether_port：客机内「系统更新」应用经 10.0.2.2:<port> 回连宿主的通道
        String append = "console=ttyAMA0 root=/dev/vda rw ip=dhcp";
        synchronized (lock) {
            if (hostPort > 0)
                append += " aether_port=" + hostPort;
        }
        cmd.add("-append"); cmd.add(append);
        cmd.add("-netdev"); cmd.add("user,id=net0");
        cmd.add("-device"); cmd.add("virtio-net-device,netdev=net0");
        cmd.add("-device"); cmd.add("virtio-gpu-pci,xres=720,yres=1600");
        cmd.add("-device"); cmd.add("virtio-keyboard-pci");
        cmd.add("-device"); cmd.add("virtio-tablet-pci");
        // VNC 端口：5900 起找空闲口，避免被别的 App 占用导致 QEMU bind 失败退出。
        int vp = pickFreePort(5900, 5999);
        synchronized (lock) { vncPort = vp; }
        cmd.add("-vnc"); cmd.add("127.0.0.1:" + (vp - 5900));
        cmd.add("-snapshot");
        cmd.add("-serial"); cmd.add("file:" + serialLog.getAbsolutePath());
        cmd.add("-display"); cmd.add("none");
        cmd.add("-monitor"); cmd.add("none");

        log("启动 QEMU: " + qemu.getName() + "（TCG 软件模拟，首次画面约 20–60 秒）");
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.directory(systemDir);
            java.util.Map<String, String> env = pb.environment();
            env.put("LD_LIBRARY_PATH", libDir);
            env.put("HOME", workDir.getAbsolutePath());
            env.put("TMPDIR", workDir.getAbsolutePath());
            env.put("QEMU_AUDIO_DRV", "none");
            pb.redirectOutput(new File(workDir, "qemu-stdout.log"));
            pb.redirectError(new File(workDir, "qemu-stderr.log"));
            Process p = pb.start();
            synchronized (lock) {
                qemuProcess = p;
                qemuPid = -1;
                qemuExitInfo = null;
                lastError = null;
            }
            log("QEMU 已启动");
            // 端口探针：QEMU 在初始化早期就会监听 VNC。8 秒还没听，说明它虽然
            // 还没退出但已经出问题了，把 stderr 提前打出来，避免用户干等一小时。
            new Thread(new Runnable() {
                @Override
                public void run() {
                    try { Thread.sleep(8000); } catch (InterruptedException e) { return; }
                    if (!isQemuRunning() || vncAlive()) return;
                    log("qemu: 8 秒仍未监听 VNC 端口，可能卡在初始化；stderr 末尾：");
                    for (String l : tailFile(new File(workDir, "qemu-stderr.log"), 20)) log("qemu: " + l);
                }
            }, "aether-vnc-probe").start();
            // 后台守护：进程退出时记录
            new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        int code = p.waitFor();
                        log("QEMU 退出 code=" + code);
                        // 关键：把 QEMU 自己的报错打到可见日志。历史上 QEMU 退出只报
                        // “code=1”，真正原因（linker 拒绝加载 / 参数错）埋在
                        // qemu-stderr.log 里，用户和开发者都看不到。
                        List<String> err = tailFile(new File(workDir, "qemu-stderr.log"), 40);
                        List<String> out = tailFile(new File(workDir, "qemu-stdout.log"), 10);
                        StringBuilder sb = new StringBuilder("code=" + code);
                        for (String l : err) { log("qemu: " + l); sb.append(" | ").append(l); }
                        for (String l : out) { log("qemu.out: " + l); }
                        if (err.isEmpty() && out.isEmpty())
                            log("qemu: （没有任何输出——通常是被 linker 拒绝或瞬间被杀）");
                        synchronized (lock) {
                            qemuExitInfo = sb.length() > 600 ? sb.substring(0, 600) : sb.toString();
                            if (lastError == null) lastError = "QEMU 退出 " + qemuExitInfo;
                        }
                    } catch (InterruptedException ignored) {}
                }
            }, "aether-qemu-waiter").start();
            return true;
        } catch (IOException e) {
            log("QEMU 启动失败: " + e.getMessage());
            synchronized (lock) { lastError = e.getMessage(); }
            return false;
        }
    }

    /** 停止 QEMU（幂等）。 */
    /**
     * 生成完整诊断日志（aether export 命令的导出内容）：
     * 状态 + 最近错误 + App 全量日志 + QEMU stderr + 客机串口尾部。
     * 崩溃监测器（Java/JS/WebView/QEMU 四层）的记录都包含在内。
     */
    public String exportDiagnostics() {
        StringBuilder sb = new StringBuilder();
        sb.append("=== AetherOS 终端诊断日志 ===\n");
        sb.append("时间: ").append(new java.util.Date()).append("\n");
        sb.append("App 版本: ").append(TAG_RELEASE).append("\n");
        sb.append("镜像版本: ").append(getVersion()).append("\n");
        sb.append("状态: installed=").append(isInstalled())
          .append(" starting=").append(isStarting())
          .append(" booted=").append(isBooted())
          .append(" qemuAlive=").append(isQemuRunning())
          .append(" vnc=").append(getVncHost()).append(':').append(getVncPort()).append("\n");
        String err = getLastError();
        if (err != null) sb.append("最近错误: ").append(err).append("\n");
        synchronized (lock) {
            if (qemuExitInfo != null) sb.append("QEMU 退出信息: ").append(qemuExitInfo).append("\n");
        }
        sb.append("\n---- App 日志（含崩溃监测记录）----\n");
        try {
            synchronized (logLines) {
                for (String l : logLines) sb.append(l).append('\n');
            }
        } catch (Throwable ignored) {}
        sb.append("\n---- QEMU stderr（尾部 60 行）----\n");
        for (String l : tailFile(new File(workDir, "qemu-stderr.log"), 60)) sb.append(l).append('\n');
        sb.append("\n---- 客机串口（尾部 60 行）----\n");
        for (String l : tailFile(serialLog, 60)) sb.append(l).append('\n');
        sb.append("=== 结束 ===\n");
        return sb.toString();
    }

    /**
     * 周期 VNC 健康探针：QEMU 运行时每 20 秒做一次完整 VNC 握手测试。
     * 只有「TCP 能连上但读不到 RFB 版本串」（服务挂死的特征）才计失败，
     * 连续 2 次失败自动重启 QEMU。端口未监听（启动期）不计失败。
     */
    private void startVncWatchdog() {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                int fails = 0;
                while (true) {
                    try { Thread.sleep(20000); } catch (InterruptedException e) { return; }
                    if (!isQemuRunning()) { fails = 0; continue; }
                    if (vncHandshakeOk()) { fails = 0; continue; }
                    fails++;
                    log("VNC 健康探针失败（第 " + fails + " 次）：TCP 可连但读不到版本串");
                    if (fails >= 2) {
                        restartQemu("VNC 握手连续 " + fails + " 次失败");
                        fails = 0;
                    }
                }
            }
        }, "aether-vnc-watchdog");
        t.setDaemon(true);
        t.start();
    }

    /** true=握手正常或端口未监听（启动期）；false=连上了但服务无响应（挂死）。 */
    private boolean vncHandshakeOk() {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress(vncHost, vncPort), 2000);
            s.setSoTimeout(4000);
            byte[] b = new byte[4];
            java.io.InputStream in = s.getInputStream();
            int off = 0;
            while (off < 4) {
                int r = in.read(b, off, 4 - off);
                if (r < 0) return false;   // 连上即 EOF：典型挂死
                off += r;
            }
            return b[0] == 'R' && b[1] == 'F' && b[2] == 'B' && b[3] == ' ';
        } catch (java.net.ConnectException e) {
            return true;                   // 端口未监听 = 启动期，不算失败
        } catch (IOException e) {
            return false;                  // 超时/重置 = 服务无响应
        } finally {
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    /** VNC 服务挂死自愈：QEMU 端口还在但永远不吐数据时，重启整个 QEMU。 */    public void restartQemu(final String why) {
        log("检测到 VNC 服务异常（" + why + "），自动重启系统…");
        stopQemu();
        new Thread(new Runnable() {
            @Override
            public void run() {
                try { Thread.sleep(2000); } catch (InterruptedException ignored) {}
                launchQemu();
            }
        }, "aether-autorestart").start();
    }

    public void stopQemu() {
        synchronized (lock) {
            if (qemuProcess != null) {
                qemuProcess.destroy();
                qemuProcess = null;
                log("已请求停止 QEMU");
            }
        }
    }

    /**
     * 解析 QEMU 实际 VNC 端口（日志里若出现 127.0.0.1:PORT 则以它为准）。
     * 解析不到时保留 launchQemu 阶段选定的端口，不要盲目回落到 5900。
     */
    public int detectVncPort() {
        File f = new File(workDir, "qemu-stderr.log");
        int found = -1;
        if (f.isFile()) {
            try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
                long len = raf.length();
                long start = Math.max(0, len - 65536);
                raf.seek(start);
                byte[] buf = new byte[(int) (len - start)];
                raf.readFully(buf);
                String s = new String(buf, "UTF-8");
                java.util.regex.Matcher m = java.util.regex.Pattern
                        .compile("127\\.0\\.0\\.1:(\\d{4,5})").matcher(s);
                while (m.find()) {
                    found = Integer.parseInt(m.group(1));
                }
            } catch (IOException ignored) {}
        }
        synchronized (lock) {
            if (found > 0) vncPort = found;
            return vncPort;
        }
    }

    // ------------------------------------------------------------------
    // 系统更新（客机内更新入口，供 #294 使用）
    // ------------------------------------------------------------------
    /**
     * 检查 GitHub 上是否有比当前版本更新的 release。
     * 返回 JSON 字符串：{"current":"phone-v8","latest":"phone-v9","has_update":true}
     */
    public String checkUpdate() {
        String current = getVersion();
        String latest = null;
        // /releases/latest 会 302 到 /releases/tag/<tag>
        for (String base : MIRRORS) {
            String url = base.replaceAll("/$", "") + "/" + REPO + "/releases/latest";
            HttpURLConnection conn = null;
            try {
                conn = (HttpURLConnection) new URL(url).openConnection();
                conn.setInstanceFollowRedirects(false);
                conn.setConnectTimeout(10000);
                conn.setReadTimeout(15000);
                conn.setRequestProperty("User-Agent", "AetherOS-Phone-App");
                int code = conn.getResponseCode();
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    String loc = conn.getHeaderField("Location");
                    if (loc != null && loc.contains("/releases/tag/")) {
                        latest = loc.substring(loc.lastIndexOf("/releases/tag/") + 14);
                        if (latest.endsWith("/")) latest = latest.substring(0, latest.length() - 1);
                    }
                } else if (code == 200) {
                    // 某些镜像不跳转，直接返回页面；从 URL 或页面提取 tag 不可靠，跳过
                }
                if (latest != null) break;
            } catch (Exception e) {
                log("更新检查通道失败: " + e.getMessage());
            } finally {
                if (conn != null) conn.disconnect();
            }
        }
        boolean hasUpdate = latest != null && !latest.equals(current);
        return "{\"current\":\"" + current + "\",\"latest\":\""
                + (latest == null ? "" : latest) + "\",\"has_update\":" + hasUpdate + "}";
    }

    /**
     * 客机内「系统更新」一键编排（#294）：停当前系统 -> 清除旧镜像 ->
     * 后台重新下载最新版并自动启动。安装/下载在 installThread 中异步进行。
     */
    public boolean applyUpdate() {
        log("系统更新：停止当前系统，准备安装新版");
        stopQemu();
        if (!resetForUpdate())
            return false;
        return startInstall();
    }

    /** 删除已安装镜像并重置状态（下次 install 重新下载最新版）。 */
    public boolean resetForUpdate() {
        stopQemu();
        synchronized (lock) {
            installing = false;
            installed = false;
            version = TAG_RELEASE;
            lastError = null;
        }
        try {
            deleteRecursively(systemDir);
            if (!systemDir.exists()) systemDir.mkdirs();
            log("已清除本地系统镜像，可重新安装最新版");
            return true;
        } catch (Exception e) {
            log("清除失败: " + e.getMessage());
            return false;
        }
    }

    private static void deleteRecursively(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) deleteRecursively(k);
        }
        f.delete();
    }

    public JSONObject statusJson() {
        JSONObject o = new JSONObject();
        try {
            boolean running = isQemuRunning();
            boolean alive = running && vncAlive();
            o.put("installed", isInstalled());
            o.put("installing", isInstalling());
            o.put("booted", alive);
            o.put("starting", running && !alive);
            o.put("qemu_running", running);
            o.put("version", getVersion());
            o.put("vnc_host", getVncHost());
            o.put("vnc_port", getVncPort());
            o.put("error", getLastError());
            synchronized (lock) { if (qemuExitInfo != null) o.put("qemu_exit", qemuExitInfo); }
        } catch (org.json.JSONException ignored) {}
        return o;
    }
}
