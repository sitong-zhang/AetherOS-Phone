package com.aetheros.terminal;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.webkit.ConsoleMessage;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * AetherOS 自包含终端 App 入口。
 *
 * 架构（无 Termux 依赖）：
 *  1. APK 内嵌 QEMU（lib/arm64-v8a/libqemu-system-aarch64.so + 依赖库）
 *  2. 本类启动内嵌 HTTP/WS 服务（AetherServer，127.0.0.1 高位端口）
 *  3. WebView 加载本地前端（终端 + 内嵌 VNC 显示器）
 *  4. 用户在终端输入 aether install → 下载系统镜像 → 自动启动 QEMU → 显示系统画面
 *
 * 可观测性约定（phone-v9）：任何时候页面没起来，都必须看到文字，而不是纯黑屏。
 *  - 启动期：原生 TextView 显示“正在启动本地服务…”
 *  - 页面加载失败：原生面板显示错误码 + 日志尾巴
 *  - 8 秒未完成加载：提示可能的原因
 */
public class MainActivity extends Activity {
    private static final String TAG = "AetherMain";

    private AetherCore core;
    private AetherServer server;
    private WebView webView;
    private TextView status;
    private int port = 0;
    private final Handler ui = new Handler(Looper.getMainLooper());

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // 全屏沉浸式 + 屏幕常亮
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();

        core = new AetherCore(getApplicationContext());
        core.log("AetherOS 终端 App 启动（自包含架构，无需 Termux）");

        // 崩溃监测器：任何 Java 线程未捕获异常都落进可见日志，屏幕页会显示。
        final Thread.UncaughtExceptionHandler prev =
                Thread.getDefaultUncaughtExceptionHandler();
        final AetherCore crashCore = core;
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread t, Throwable e) {
                try {
                    crashCore.log("[Java 崩溃] 线程 " + t.getName() + ": " + e);
                    StackTraceElement[] st = e.getStackTrace();
                    for (int i = 0; i < Math.min(st.length, 12); i++) {
                        crashCore.log("    at " + st[i]);
                    }
                    if (e.getCause() != null) crashCore.log("[Java 崩溃] cause: " + e.getCause());
                } catch (Throwable ignored) {}
                if (prev != null) prev.uncaughtException(t, e);
            }
        });

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF05070D);

        webView = new WebView(this);
        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setCacheMode(WebSettings.LOAD_NO_CACHE);
        ws.setMediaPlaybackRequiresUserGesture(false);
        ws.setTextZoom(100);
        // targetSdk 28+ 起 WebView 默认禁明文；本地服务是 http://127.0.0.1，需放行
        ws.setAllowFileAccess(false);
        ws.setAllowContentAccess(false);
        webView.setBackgroundColor(0xFF05070D);
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url) {
                // 前端「导出日志」入口：aether://export-log
                if (url != null && url.startsWith("aether://export-log")) {
                    exportLog();
                    return true;
                }
                // 只允许本地服务内导航
                if (url.startsWith("http://127.0.0.1:") || url.startsWith("http://localhost:")) {
                    return false;
                }
                return true;
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                ui.removeCallbacks(loadWatchdog);
                hideStatus();
                core.log("前端已加载: " + url);
            }

            // 崩溃监测器：WebView 渲染进程崩溃（JS 层捕获不到）时重建页面
            @Override
            public boolean onRenderProcessGone(WebView view, android.webkit.RenderProcessGoneDetail detail) {
                core.log("[WebView 崩溃] 渲染进程消失，didCrash="
                        + (detail == null ? "?" : detail.didCrash()) + "，正在重建页面");
                try {
                    ViewGroup p = (ViewGroup) view.getParent();
                    if (p != null) { p.removeView(view); view.destroy(); }
                } catch (Throwable ignored) {}
                ui.post(new Runnable() {
                    @Override
                    public void run() { recreate(); }
                });
                return true; // 已处理，不再让 App 直接被杀
            }

            @SuppressWarnings("deprecation")
            @Override
            public void onReceivedError(WebView view, int errorCode,
                                        String description, String failingUrl) {
                ui.removeCallbacks(loadWatchdog);
                showStatus("前端加载失败\n\n错误 " + errorCode + "：" + description
                        + "\n" + failingUrl + "\n\n" + tail());
            }
        });
        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage m) {
                core.log("JS: " + m.message());
                return true;
            }
        });

        status = new TextView(this);
        status.setTextColor(0xFFC9D6E8);
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER);
        status.setPadding(56, 56, 56, 56);
        status.setText("正在启动本地服务…");

        root.addView(webView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(status, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);

        // 后台启动内嵌服务
        new Thread(new Runnable() {
            @Override
            public void run() { startServer(); }
        }, "aether-boot").start();
    }

    /** 页面 8 秒还没起来，把已知信息摊开给人看，别让用户对着黑屏猜。 */
    private final Runnable loadWatchdog = new Runnable() {
        @Override
        public void run() {
            showStatus("前端加载超时（8 秒）\n\n" + tail()
                    + "\n\n请截图本页反馈给开发者。");
        }
    };

    private void startServer() {
        try {
            // 端口探测 18080–18180
            for (int p = 18080; p <= 18180; p++) {
                if (portFree(p)) {
                    try {
                        server = new AetherServer(getApplicationContext(), core, p);
                        server.start();
                        port = p;
                        break;
                    } catch (Throwable e) {
                        core.log("端口 " + p + " 绑定失败: " + e);
                    }
                }
            }
        } catch (Throwable e) {
            core.log("服务启动异常: " + e);
        }
        if (port == 0) {
            showStatus("服务启动失败：18080–18180 无可用端口。\n\n" + tail());
            return;
        }
        final String url = "http://127.0.0.1:" + port + "/";
        // 端口下发：客机经 aether_port= 内核参数拿到它，10.0.2.2:<port> 回连宿主
        core.setHostPort(port);
        core.log("内嵌服务端口 " + port + "（客机更新通道 10.0.2.2:" + port + "）");
        core.log("加载前端: " + url);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                status.setText("正在加载界面…\n" + url);
                webView.loadUrl(url);
                ui.postDelayed(loadWatchdog, 8000);
            }
        });
    }

    /**
     * 导出诊断日志：生成完整文本（App 日志含崩溃监测、QEMU stderr、
     * 客机串口），存到私有目录并弹出系统分享面板——用户可直接把日志
     * 通过微信/QQ/邮件发出去，截图之外的第二条排错通道。
     *
     * phone-v18：整体挪到后台线程。exportDiagnostics() 里 isBooted() 会做
     * 端口探测（socket connect），在主线程跑就是 NetworkOnMainThreadException
     * ——v16/v17 的导出按钮四连败就是这个。分享面板回到 UI 线程弹。
     */
    private void exportLog() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    final String text = core.exportDiagnostics();
                    java.io.File dir = new java.io.File(getFilesDir(), "exports");
                    dir.mkdirs();
                    final java.io.File f = new java.io.File(dir,
                            "aether-log-" + android.text.format.DateFormat.format("MMdd-HHmmss", System.currentTimeMillis()) + ".txt");
                    java.io.FileOutputStream fo = new java.io.FileOutputStream(f);
                    fo.write(text.getBytes("UTF-8"));
                    fo.close();
                    core.log("日志已导出: " + f.getAbsolutePath() + "（" + text.length() + " 字符）");

                    final android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_SEND);
                    i.setType("text/plain");
                    i.putExtra(android.content.Intent.EXTRA_SUBJECT, "AetherOS 诊断日志");
                    // 分享面板对超长文本兼容性差，超限时给摘要 + 文件路径
                    if (text.length() > 500000) {
                        i.putExtra(android.content.Intent.EXTRA_TEXT,
                                text.substring(0, 500000) + "\n…（过长已截断，完整文件在 " + f.getAbsolutePath() + "）");
                    } else {
                        i.putExtra(android.content.Intent.EXTRA_TEXT, text);
                    }
                    ui.post(new Runnable() {
                        @Override
                        public void run() {
                            try {
                                startActivity(android.content.Intent.createChooser(i, "导出 AetherOS 日志"));
                            } catch (Throwable t2) {
                                core.log("分享面板弹出失败: " + t2);
                            }
                        }
                    });
                } catch (Throwable t) {
                    core.log("导出日志失败: " + t);
                }
            }
        }, "aether-export").start();
    }

    private String tail() {
        StringBuilder sb = new StringBuilder("—— 最近日志 ——\n");
        java.util.List<String> lines = core.tailLog(12);
        synchronized (lines) {
            for (String l : lines) sb.append(l).append('\n');
        }
        return sb.toString();
    }

    private void showStatus(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (status == null) return;
                status.setText(text);
                status.setVisibility(View.VISIBLE);
                status.bringToFront();
                if (webView != null) webView.setVisibility(View.INVISIBLE);
            }
        });
    }

    private void hideStatus() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (status != null) status.setVisibility(View.GONE);
                if (webView != null) webView.setVisibility(View.VISIBLE);
            }
        });
    }

    private boolean portFree(int p) {
        Socket s = new Socket();
        try {
            s.connect(new InetSocketAddress("127.0.0.1", p), 200);
            return false; // 能连上说明被占用
        } catch (Exception e) {
            return true;
        } finally {
            try { s.close(); } catch (Exception ignored) {}
        }
    }

    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decor.setSystemUiVisibility(flags);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    public void onBackPressed() {
        // 前端自行处理返回；交给 WebView 历史
        if (webView != null && webView.canGoBack()) {
            webView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacks(loadWatchdog);
        if (server != null) server.stop();
        if (core != null) core.stopQemu();
        if (webView != null) {
            webView.destroy();
            webView = null;
        }
    }
}
