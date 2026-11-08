package dev.aether.vm;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * 崩溃报告弹窗（自研）。
 * 声明在 :crash 独立进程 —— 主进程已经崩掉时，它依然能被拉起来显示错误。
 * 纯 Java 代码构建 UI，不依赖任何额外资源或第三方库。
 *
 * 【1.0.7】旧版按钮改为「保存文件」：点击把完整报告写成 .txt 并弹出系统
 * 分享面板，用户把原文件发给开发者，不再经过剪贴板（粘贴会被长度截断）。
 */
public final class CrashReportActivity extends Activity {
    private String report;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        BootTrace.mark("CrashReportActivity.onCreate（:crash 独立进程）");

        report = getIntent().getStringExtra(CrashHandler.EXTRA_TEXT);
        if (report == null || report.length() == 0) report = CrashHandler.readLatest(this);
        buildUi();

        // 已展示，清掉「未读」标记；latest.txt 保留，随时可再次进来保存
        try { CrashHandler.pendingFile(this).delete(); } catch (Throwable ignored) {}
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF0B0E12);
        root.setPadding(20, 26, 20, 20);

        TextView title = new TextView(this);
        title.setText("Aether VM · 崩溃报告");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(20);
        title.setPadding(0, 0, 0, 10);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("下面是应用崩溃时的具体错误，报告已完整生成。\n"
                + "★ 点击「保存文件」将完整报告存为 .txt，并在系统分享面板中\n"
                + "   发送该文件 —— 不经过剪贴板，内容不做截断。\n"
                + "报告同时会自动落到「下载/AetherVM/」目录。文本也支持长按选择。");
        hint.setTextColor(0xFF9FB3C8);
        hint.setTextSize(13);
        hint.setPadding(0, 0, 0, 16);
        root.addView(hint);

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(0xFF060809);
        TextView body = new TextView(this);
        body.setText(report);
        body.setTypeface(Typeface.MONOSPACE);
        body.setTextSize(11);
        body.setTextColor(0xFFD7E2EE);
        body.setTextIsSelectable(true);
        body.setPadding(16, 16, 16, 16);
        sv.addView(body);
        root.addView(sv, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, 16, 0, 0);

        Button save = makeButton("保存文件", 0xFF2D6AE0);
        Button restart = makeButton("重启应用", 0xFF2E7D5B);
        Button close = makeButton("关闭", 0xFF3A4450);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(4, 0, 4, 0);
        row.addView(save, lp);
        row.addView(restart, lp);
        row.addView(close, lp);
        root.addView(row);

        save.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                ReportSaver.saveAndShare(CrashReportActivity.this, report);
            }
        });
        restart.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { restartApp(); }
        });
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { finish(); }
        });

        setContentView(root);
    }

    private Button makeButton(String text, int color) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(0xFFFFFFFF);
        b.setTextSize(13);
        b.setAllCaps(false);
        b.setBackgroundColor(color);
        b.setPadding(8, 8, 8, 8);
        return b;
    }

    private void restartApp() {
        try { CrashHandler.pendingFile(this).delete(); } catch (Throwable ignored) {}
        try {
            Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
            if (i != null) {
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
            }
        } catch (Throwable ignored) {}
        finish();
    }
}
