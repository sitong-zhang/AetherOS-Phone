package dev.aether.vm;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

/**
 * 系统更新界面（自研，纯 Java，无 androidx 依赖）。
 *
 * 检查 GitHub 公开 Release、显示版本差异、下载并做 SHA-256 校验，校验通过后
 * 通过系统安装器升级。不携带任何访问凭证；公开 Release 的读取不需要认证。
 * 所有文案面向使用者作客观陈述，不含调试或对话语气。
 */
public final class UpdateActivity extends Activity
        implements UpdateManager.Callback, UpdateManager.UiBridge {

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView info;
    private TextView progress;
    private Button checkBtn;
    private Button installBtn;
    private UpdateManager.Info latest;
    private File downloaded;
    private boolean alive;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        alive = true;
        setTitle("系统更新");
        setContentView(buildUi());
        renderIdle();
        UpdateManager.check(this, this, this);
    }

    @Override
    protected void onDestroy() {
        alive = false;
        super.onDestroy();
    }

    @Override public void post(Runnable r) { ui.post(r); }
    @Override public boolean isAlive() { return alive; }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF0B0E12);
        root.setPadding(20, 24, 20, 20);

        TextView title = new TextView(this);
        title.setText("系统更新");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(22);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, 0, 0, 6);
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText("新版本通过公开的 GitHub Release 分发，可在应用内完成下载与安装。");
        sub.setTextColor(0xFF9FB3C8);
        sub.setTextSize(12);
        sub.setGravity(Gravity.CENTER);
        sub.setPadding(0, 0, 0, 16);
        root.addView(sub);

        info = new TextView(this);
        info.setTextColor(0xFFD7E2EE);
        info.setTextSize(13);
        info.setTextIsSelectable(true);
        info.setPadding(14, 14, 14, 14);
        info.setBackgroundColor(0xFF11161E);
        ScrollView sv = new ScrollView(this);
        sv.addView(info);
        root.addView(sv, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        progress = new TextView(this);
        progress.setTextColor(0xFF7FD1FF);
        progress.setTextSize(12);
        progress.setTypeface(android.graphics.Typeface.MONOSPACE);
        progress.setPadding(0, 12, 0, 8);
        root.addView(progress);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, 8, 0, 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(4, 0, 4, 0);

        checkBtn = button("检查更新", 0xFF2456C8);
        installBtn = button("下载并安装", 0xFF2E7D5B);
        Button close = button("返回", 0xFF3A4450);
        row.addView(checkBtn, lp);
        row.addView(installBtn, lp);
        row.addView(close, lp);
        root.addView(row, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        checkBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                latest = null;
                downloaded = null;
                renderIdle();
                UpdateManager.check(UpdateActivity.this, UpdateActivity.this, UpdateActivity.this);
            }
        });
        installBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (downloaded != null) {
                    requestInstall(downloaded);
                } else if (latest != null) {
                    UpdateManager.download(UpdateActivity.this, latest,
                            UpdateActivity.this, UpdateActivity.this);
                }
            }
        });
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { finish(); }
        });
        return root;
    }

    private void renderIdle() {
        checkBtn.setEnabled(true);
        installBtn.setEnabled(false);
        progress.setText("");
        info.setText("当前版本：" + UpdateManager.localVersion(this)
                + "\n\n正在检查公开发布的最新版本 …");
    }

    @Override
    public void onCheckStart() {
        checkBtn.setEnabled(false);
        installBtn.setEnabled(false);
        progress.setText("检查中 …");
    }

    @Override
    public void onCheckDone(UpdateManager.Info in, String error) {
        checkBtn.setEnabled(true);
        progress.setText("");
        if (error != null) {
            info.setText("当前版本：" + UpdateManager.localVersion(this)
                    + "\n\n检查未完成：" + error
                    + "\n\n可稍后重试，或在网络可用时重新检查。");
            return;
        }
        if (in == null) {
            info.setText("当前版本：" + UpdateManager.localVersion(this)
                    + "\n\n未获取到版本信息。");
            return;
        }
        latest = in;
        StringBuilder b = new StringBuilder();
        b.append("当前版本：").append(in.localVersion).append('\n');
        b.append("最新版本：").append(in.tag).append('\n');
        b.append("安装包体积：").append(in.size > 0
                ? String.format(Locale.CHINA, "%.1f MB", in.size / 1048576.0) : "未提供").append('\n');
        b.append("状态：").append(in.newer ? "存在可用更新" : "已是最新版本").append("\n\n");
        if (!in.name.isEmpty()) b.append(in.name).append("\n\n");
        if (!in.body.isEmpty()) b.append(clip(in.body, 1200)).append('\n');
        info.setText(b.toString());
        installBtn.setEnabled(in.newer);
    }

    @Override
    public void onProgress(int percent, long done, long total) {
        if (percent >= 0)
            progress.setText(String.format(Locale.CHINA, "下载中 %d%%  (%.1f / %.1f MB)",
                    percent, done / 1048576.0, total / 1048576.0));
        else
            progress.setText(String.format(Locale.CHINA, "下载中  %.1f MB", done / 1048576.0));
    }

    @Override
    public void onDownloadDone(File apk, String error) {
        if (error != null) {
            progress.setText("下载未完成：" + error);
            installBtn.setEnabled(true);
            return;
        }
        downloaded = apk;
        progress.setText("下载完成，SHA-256 校验通过，可开始安装。");
        installBtn.setEnabled(true);
        installBtn.setText("开始安装");
        requestInstall(apk);
    }

    /** Android 8.0 起需先取得「安装未知应用」授权，否则系统安装器会拒绝。 */
    private void requestInstall(File apk) {
        if (Build.VERSION.SDK_INT >= 26) {
            try {
                if (!getPackageManager().canRequestPackageInstalls()) {
                    progress.setText("需要先允许本应用安装更新包。");
                    Intent i = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            Uri.parse("package:" + getPackageName()));
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(i);
                    return;
                }
            } catch (Throwable ignored) {}
        }
        try {
            UpdateManager.install(this, apk);
            progress.setText("已交给系统安装器，按提示完成升级。");
        } catch (Throwable t) {
            progress.setText("无法拉起安装器：" + t);
        }
    }

    private Button button(String text, int color) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(0xFFFFFFFF);
        b.setTextSize(12);
        b.setAllCaps(false);
        b.setBackgroundColor(color);
        b.setPadding(6, 8, 6, 8);
        return b;
    }

    private static String clip(String s, int max) {
        return s.length() <= max ? s : s.substring(0, max) + " …";
    }
}
