package com.aetheros.terminal;

import android.content.Context;

import java.io.*;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * 手写 HTTP/1.1 服务器（Android 无 com.sun.net.httpserver，纯 ServerSocket 实现）。
 *
 * 路由：
 *   GET  /                     -> index.html
 *   GET  /app.js 等静态资源    -> assets/
 *   GET  /api/status           -> JSON 状态
 *   GET  /api/logs             -> SSE 日志流（安装 + 串口）
 *   POST /api/install          -> 触发安装
 *   POST /api/boot             -> 启动 QEMU（已安装时）
 *   POST /api/stop             -> 停止 QEMU
 *   POST /api/reset            -> 清除镜像（供系统更新用）
 *   GET  /ws/vnc               -> WebSocket ⇄ VNC 桥
 */
public class AetherServer {
    private final Context ctx;
    private final AetherCore core;
    private final int port;
    private ServerSocket server;
    private ExecutorService pool;
    private volatile boolean running = false;

    public AetherServer(Context ctx, AetherCore core, int port) {
        this.ctx = ctx;
        this.core = core;
        this.port = port;
    }

    public int getPort() { return port; }

    public void start() throws IOException {
        server = new ServerSocket(port, 64, java.net.InetAddress.getByName("127.0.0.1"));
        server.setReuseAddress(true);
        running = true;
        pool = Executors.newCachedThreadPool(new ThreadFactory() {
            private final AtomicInteger n = new AtomicInteger(1);
            @Override
            public Thread newThread(Runnable r) {
                return new Thread(r, "aether-http-" + n.getAndIncrement());
            }
        });
        core.log("HTTP 服务已启动: http://127.0.0.1:" + port);
        pool.execute(new Runnable() {
            @Override
            public void run() { acceptLoop(); }
        });
    }

    public void stop() {
        running = false;
        try { if (server != null) server.close(); } catch (IOException ignored) {}
        if (pool != null) pool.shutdownNow();
    }

    private void acceptLoop() {
        while (running) {
            try {
                final Socket s = server.accept();
                s.setTcpNoDelay(true);
                pool.execute(new Runnable() {
                    @Override
                    public void run() { handle(s); }
                });
            } catch (IOException e) {
                if (running) core.log("accept 异常: " + e.getMessage());
            }
        }
    }

    // ------------------------------------------------------------------
    // 请求处理
    // ------------------------------------------------------------------
    private void handle(Socket s) {
        try {
            s.setSoTimeout(30000);
            InputStream in = new BufferedInputStream(s.getInputStream(), 16384);
            OutputStream out = new BufferedOutputStream(s.getOutputStream(), 65536);

            String requestLine = readLine(in);
            if (requestLine == null || requestLine.isEmpty()) return;
            String[] parts = requestLine.split(" ");
            if (parts.length < 2) return;
            String method = parts[0];
            String target = parts[1];
            String path = target;
            int q = path.indexOf('?');
            if (q >= 0) path = path.substring(0, q);

            // 读头
            int contentLength = 0;
            boolean upgrade = false;
            String wsKey = null;
            String line;
            while ((line = readLine(in)) != null && !line.isEmpty()) {
                int idx = line.indexOf(':');
                if (idx <= 0) continue;
                String name = line.substring(0, idx).trim();
                String val = line.substring(idx + 1).trim();
                if (name.equalsIgnoreCase("Content-Length")) {
                    try { contentLength = Integer.parseInt(val); } catch (NumberFormatException ignored) {}
                } else if (name.equalsIgnoreCase("Upgrade")) {
                    upgrade = val.toLowerCase().contains("websocket");
                } else if (name.equalsIgnoreCase("Sec-WebSocket-Key")) {
                    wsKey = val;
                }
            }

            if (upgrade && path.startsWith("/ws/vnc")) {
                // 系统没就绪时不要把连接挂在这里：WsBridge 会阻塞 5 秒才失败，
                // 前端只看到“连接断开”，根本不知道是系统没启动，于是无限重试刷屏。
                if (!core.isBooted()) {
                    String why = core.isQemuRunning()
                            ? "系统正在启动，画面还没就绪，请稍候"
                            : ("系统未启动。" + (core.getLastError() == null ? "" : " " + core.getLastError()));
                    core.log("屏幕请求被拒绝：" + why);
                    sendText(out, 503, why);
                    return;
                }
                int vp = core.detectVncPort();
                new WsBridge(s, wsKey, core.getVncHost(), vp, core).run();
                return;
            }

            // 路由
            if (method.equals("GET") && (path.equals("/") || path.equals("/index.html"))) {
                sendAsset(out, "index.html", "text/html; charset=utf-8");
            } else if (method.equals("GET") && path.equals("/api/status")) {
                sendJson(out, core.statusJson().toString());
            } else if (method.equals("GET") && path.equals("/api/logs")) {
                sseLogs(s, in, out);
                return; // SSE 自行管理连接
            } else if (method.equals("GET") && path.equals("/api/update-check")) {
                sendJson(out, core.checkUpdate());
            } else if (method.equals("POST") && path.equals("/api/install")) {
                drain(in, contentLength);
                boolean started = core.startInstall();
                sendJson(out, "{\"started\":" + started + "}");
            } else if (method.equals("POST") && path.equals("/api/boot")) {
                drain(in, contentLength);
                boolean ok = core.launchQemu();
                sendJson(out, "{\"ok\":" + ok + "}");
            } else if (method.equals("POST") && path.equals("/api/stop")) {
                drain(in, contentLength);
                core.stopQemu();
                sendJson(out, "{\"ok\":true}");
            } else if (method.equals("POST") && path.equals("/api/reset")) {
                drain(in, contentLength);
                boolean ok = core.resetForUpdate();
                sendJson(out, "{\"ok\":" + ok + "}");
            } else if (method.equals("POST") && path.equals("/api/update")) {
                // 客机内「系统更新」一键更新（#294）：停->清->装->自动启动
                drain(in, contentLength);
                boolean started = core.applyUpdate();
                sendJson(out, "{\"started\":" + started + "}");
            } else if (method.equals("POST") && path.equals("/api/crash")) {
                // 崩溃监测器的前端上报通道：JS 错误落进可见日志，屏幕页可截图定位
                String body = readBody(in, contentLength);
                core.log("[前端崩溃] " + body);
                sendJson(out, "{\"ok\":true}");
            } else if (method.equals("GET")) {
                // 静态资源（白名单扩展名，防路径穿越）
                String name = new File(path).getName();
                if (!name.matches("[A-Za-z0-9._-]+") || name.contains("..")) {
                    sendText(out, 403, "forbidden");
                } else {
                    String ctype = ctypeOf(name);
                    if (ctype == null) sendText(out, 403, "forbidden");
                    else sendAsset(out, name, ctype);
                }
            } else {
                sendText(out, 404, "not found");
            }
            out.flush();
        } catch (Throwable e) {
            core.log("HTTP 处理异常: " + e);
            try {
                OutputStream o = s.getOutputStream();
                sendText(o, 500, "server error: " + e);
                o.flush();
            } catch (Throwable ignored) {}
        } finally {
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    private static String readLine(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') break;
            if (c != '\r') sb.append((char) c);
            if (sb.length() > 8192) break;
        }
        if (c < 0 && sb.length() == 0) return null;
        return sb.toString();
    }

    private static void drain(InputStream in, int n) throws IOException {
        long remain = n;
        while (remain > 0) {
            long skipped = in.skip(remain);
            if (skipped <= 0) { if (in.read() < 0) break; skipped = 1; }
            remain -= skipped;
        }
    }

    /** 读取请求体（上限 8KB，足够放错误信息），转成可读文本。 */
    private static String readBody(InputStream in, int n) throws IOException {
        if (n <= 0) return "";
        int len = Math.min(n, 8192);
        byte[] buf = new byte[len];
        int off = 0;
        while (off < len) {
            int r = in.read(buf, off, len - off);
            if (r < 0) break;
            off += r;
        }
        return new String(buf, 0, off, StandardCharsets.UTF_8);
    }

    private static String ctypeOf(String name) {
        int d = name.lastIndexOf('.');
        if (d < 0) return null;
        String ext = name.substring(d).toLowerCase();
        switch (ext) {
            case ".html": return "text/html; charset=utf-8";
            case ".js": return "application/javascript; charset=utf-8";
            case ".css": return "text/css; charset=utf-8";
            case ".json": case ".webmanifest": return "application/manifest+json";
            case ".png": return "image/png";
            case ".jpg": case ".jpeg": return "image/jpeg";
            case ".svg": return "image/svg+xml";
            case ".ico": return "image/x-icon";
            default: return null;
        }
    }

    private void sendAsset(OutputStream out, String name, String ctype) throws IOException {
        byte[] data;
        try (InputStream is = ctx.getAssets().open(name)) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[65536];
            int n;
            while ((n = is.read(buf)) > 0) bos.write(buf, 0, n);
            data = bos.toByteArray();
        } catch (IOException e) {
            // 绝不能静默：旧版 APK 把 assets 打到了归档根目录，这里返回 404 纯文本，
            // WebView 用深色背景 + 默认黑字渲染 => 用户看到的就是“纯黑屏、什么都点不了”。
            core.log("资源缺失: " + name + " (" + e.getMessage() + ")，请检查 APK 的 assets 打包");
            sendHtml(out, 500,
                    "<!doctype html><html><head><meta charset='utf-8'>"
                    + "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    + "</head><body style='margin:0;padding:24px;background:#0b0f18;"
                    + "color:#e6edf7;font:16px/1.6 monospace'>"
                    + "<h3 style='color:#ff7b72;margin:0 0 12px'>前端资源缺失</h3>"
                    + "<p>APK 内找不到 <b>assets/" + name + "</b>。</p>"
                    + "<p>这是打包路径错误（旧版 16.3 安装器的问题），请安装新版安装器。</p>"
                    + "<p style='color:#8b98a9'>error: " + escapeHtml(String.valueOf(e)) + "</p>"
                    + "</body></html>");
            return;
        }
        out.write(("HTTP/1.1 200 OK\r\n"
                + "Content-Type: " + ctype + "\r\n"
                + "Content-Length: " + data.length + "\r\n"
                + "Cache-Control: no-cache\r\n"
                + "Access-Control-Allow-Origin: *\r\n"
                + "\r\n").getBytes(StandardCharsets.ISO_8859_1));
        out.write(data);
    }

    private void sendJson(OutputStream out, String json) throws IOException {
        byte[] data = json.getBytes(StandardCharsets.UTF_8);
        out.write(("HTTP/1.1 200 OK\r\n"
                + "Content-Type: application/json; charset=utf-8\r\n"
                + "Content-Length: " + data.length + "\r\n"
                + "Access-Control-Allow-Origin: *\r\n"
                + "\r\n").getBytes(StandardCharsets.ISO_8859_1));
        out.write(data);
    }

    private void sendHtml(OutputStream out, int code, String html) throws IOException {
        byte[] data = html.getBytes(StandardCharsets.UTF_8);
        String reason = code == 500 ? "Internal Server Error" : "OK";
        out.write(("HTTP/1.1 " + code + " " + reason + "\r\n"
                + "Content-Type: text/html; charset=utf-8\r\n"
                + "Content-Length: " + data.length + "\r\n"
                + "Cache-Control: no-cache\r\n"
                + "\r\n").getBytes(StandardCharsets.ISO_8859_1));
        out.write(data);
    }

    private static String escapeHtml(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    private void sendText(OutputStream out, int code, String body) throws IOException {
        byte[] data = body.getBytes(StandardCharsets.UTF_8);
        String reason = code == 404 ? "Not Found" : code == 403 ? "Forbidden" : "Error";
        out.write(("HTTP/1.1 " + code + " " + reason + "\r\n"
                + "Content-Type: text/plain; charset=utf-8\r\n"
                + "Content-Length: " + data.length + "\r\n"
                + "\r\n").getBytes(StandardCharsets.ISO_8859_1));
        out.write(data);
    }

    // ------------------------------------------------------------------
    // SSE 日志流：安装日志 + 客机串口日志，400ms 轮询 tail
    // ------------------------------------------------------------------
    private void sseLogs(Socket s, InputStream in, OutputStream out) {
        try {
            s.setSoTimeout(0);
            out.write(("HTTP/1.1 200 OK\r\n"
                    + "Content-Type: text/event-stream\r\n"
                    + "Cache-Control: no-cache\r\n"
                    + "Connection: keep-alive\r\n"
                    + "Access-Control-Allow-Origin: *\r\n"
                    + "\r\n").getBytes(StandardCharsets.ISO_8859_1));
            out.flush();

            // 先发历史（最后 100 行）
            List<String> hist = core.allLog();
            int sent = hist.size();
            int from = Math.max(0, sent - 100);
            for (String l : hist.subList(from, sent)) writeSse(out, l);
            out.flush();

            while (running && !s.isClosed()) {
                Thread.sleep(400);
                // 日志增量：只推自上次轮询起新增的行（#302：修复每轮重发尾部 30 行的 flooding）
                List<String> all = core.allLog();
                if (all.size() < sent) {
                    // 环形缓冲截断（>500 行）：重发尾部 100 行并重置游标
                    sent = all.size();
                    int from2 = Math.max(0, sent - 100);
                    for (String l : all.subList(from2, sent)) writeSse(out, l);
                } else if (all.size() > sent) {
                    for (String l : all.subList(sent, all.size())) writeSse(out, l);
                    sent = all.size();
                }
                // 串口增量
                String serial = core.serialTail(8192);
                if (serial != null && !serial.isEmpty()) {
                    // serialTail 返回尾部块，仅当变长时才推（简单去重：与上次不同且更长）
                    String cur = lastSerial;
                    if (!serial.equals(cur)) {
                        String delta = serial.startsWith(cur) && !cur.isEmpty()
                                ? serial.substring(cur.length()) : serial;
                        for (String l : delta.split("\n")) {
                            if (!l.trim().isEmpty()) writeSse(out, l);
                        }
                        lastSerial = serial;
                    }
                }
                out.flush();
                if (out instanceof BufferedOutputStream) {
                    // 确保心跳感知断开
                    if (s.getOutputStream() == null) break;
                }
            }
        } catch (Exception e) {
            core.log("SSE 结束: " + e.getMessage());
        }
    }

    private String lastSerial = "";

    private void writeSse(OutputStream out, String line) throws IOException {
        String safe = line.replace("\r", " ").replace("\n", " ");
        byte[] data = ("data: " + safe + "\n\n").getBytes(StandardCharsets.UTF_8);
        out.write(data);
    }
}
