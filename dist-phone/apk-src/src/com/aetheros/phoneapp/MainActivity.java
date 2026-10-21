package com.aetheros.phoneapp;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.graphics.Bitmap;
import android.graphics.Color;

import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * AetherOS Phone App —— WebView 套壳。
 * 加载本机 App 服务 http://127.0.0.1:<port>（phone-app.sh 启动的 server.py）。
 * 端口自动探测：依次尝试 8080 / 8081 / 8082。
 */
public class MainActivity extends Activity {

    private WebView webView;
    private static final int[] CANDIDATE_PORTS = {8080, 8081, 8082};
    private boolean errorPageShown = false;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // 常亮 + 沉浸式全屏
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();

        webView = new WebView(this);
        webView.setBackgroundColor(Color.BLACK);
        WebSettings s = webView.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setCacheMode(WebSettings.LOAD_DEFAULT);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);

        webView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                errorPageShown = false;
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                // 外链也走 WebView 内部（系统内自包含体验）
                return false;
            }

            @Override
            public void onReceivedError(WebView view, int errorCode,
                                        String description, String failingUrl) {
                if (!errorPageShown) {
                    errorPageShown = true;
                    view.loadDataWithBaseURL(null, buildHelpHtml(description),
                            "text/html", "utf-8", null);
                }
            }
        });

        setContentView(webView);
        detectPortAndLoad();
    }

    /** 依次探测候选端口，加载第一个在监听的服务；全都不通则显示帮助页。 */
    private void detectPortAndLoad() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final int port = probePort();
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (port > 0) {
                            webView.loadUrl("http://127.0.0.1:" + port + "/");
                        } else {
                            webView.loadDataWithBaseURL(null, buildHelpHtml("未找到 App 服务"),
                                    "text/html", "utf-8", null);
                        }
                    }
                });
            }
        }).start();
    }

    private int probePort() {
        for (int p : CANDIDATE_PORTS) {
            Socket sock = new Socket();
            try {
                sock.connect(new InetSocketAddress("127.0.0.1", p), 400);
                return p;
            } catch (Exception ignored) {
                // 继续下一个
            } finally {
                try { sock.close(); } catch (Exception ignored) { }
            }
        }
        return -1;
    }

    private String buildHelpHtml(String reason) {
        try {
            InputStream in = getAssets().open("help.html");
            byte[] buf = new byte[in.available()];
            in.read(buf);
            in.close();
            String html = new String(buf, "UTF-8");
            return html.replace("%REASON%", reason);
        } catch (Exception e) {
            return "<html><body style=\"background:#05070d;color:#9fb4ff;font-family:sans-serif;padding:24px\">"
                    + "<h2>未连接到 AetherOS App 服务</h2><p>" + reason + "</p>"
                    + "<p>请在 Termux 运行：<br><code>curl -fsSL "
                    + "https://gh-proxy.com/https://github.com/sitong-zhang/AetherOS-Phone"
                    + "/releases/latest/download/phone-app.sh | sh</code></p></body></html>";
        }
    }

    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) {
            webView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        if (webView != null) {
            webView.destroy();
            webView = null;
        }
        super.onDestroy();
    }
}
