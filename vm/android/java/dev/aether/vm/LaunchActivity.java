package dev.aether.vm;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * 纯 Java 启动分发入口（自研）。
 * 不加载任何 native 库、不继承 SDLActivity，因此无论 QEMU 引擎崩得多快，
 * 用户点开图标都一定能看到这个界面 —— 这是「零反馈闪退」的兜底锚点。
 *
 * 【1.0.5】有未读报告时【内联显示在本 Activity】，不再依赖可能被华为/EMUI
 * 后台启动限制拦掉的 :crash 跨进程弹窗。
 * 【1.0.7】旧版按钮改为「保存文件」：报告完整写成 .txt 并弹出系统分享，
 * 用户把原文件发给开发者，彻底绕开剪贴板粘贴被截断的问题。
 *   - 「重试进入系统」清标记后重新启动引擎（每次崩溃都会留报告）；
 *   - 「关闭」退出；本页不会自动跳转 BootActivity。
 * 无报告时才正常分发到 BootActivity（保持「装上打开一键进系统」的体验）。
 */
public final class LaunchActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        BootTrace.mark("LaunchActivity.onCreate（纯 Java 入口，无 native 依赖）");

        // ★ 顺序至关重要：必须在任何 markStage 之前合成，否则会覆盖掉上次
        //   崩溃遗留的 boot_stage.txt，报告里的「最后停留阶段」就失真了。
        CrashHandler.synthesizeOnColdStart(this);

        if (CrashHandler.hasPending(this)) {
            BootTrace.mark("检测到上次崩溃/未正常收尾报告 -> 停在本页内联显示（不自动进引擎）");
            String report = CrashHandler.readLatest(this);
            // 报告页分支绝不写阶段标记：本次没有启动引擎，不能留下「未完成」痕迹。
            try { CrashHandler.pendingFile(this).delete(); } catch (Throwable ignored) {}
            showReportPage(report);
            return;
        }

        CrashHandler.markLaunchStart(this);
        CrashHandler.markStage(this, "STAGE_START_BOOT_ACTIVITY");
        BootTrace.mark("启动 BootActivity（SDL 显示 / QEMU 引擎）");
        setContentView(buildSplash());
        startActivity(new Intent(this, BootActivity.class));
        finish();
    }

    private View buildSplash() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(0xFF0B0E12);
        TextView tv = new TextView(this);
        tv.setText("Aether VM\n正在启动虚拟机…");
        tv.setTextColor(0xFF9FB3C8);
        tv.setTextSize(18);
        tv.setGravity(Gravity.CENTER);
        root.addView(tv);
        return root;
    }

    /** 内联报告页：全屏可滚动文本 + 保存文件/重试/关闭。纯 Java，无 native 依赖。 */
    private void showReportPage(final String report) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF0B0E12);
        root.setPadding(18, 24, 18, 18);

        TextView title = new TextView(this);
        title.setText("Aether VM · 上次启动诊断");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(19);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding(0, 0, 0, 8);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("上次启动未能正常收尾，完整报告已生成（本页不会自动启动引擎）。\n"
                + "★ 点「保存文件」把完整报告存成 .txt，并在弹出的发送面板里\n"
                + "   把该文件发给开发者 —— 不再走剪贴板，不会被粘贴长度截断。\n"
                + "报告同时会自动落到「下载/AetherVM/」目录，也可在文件管理器找到。");
        hint.setTextColor(0xFF9FB3C8);
        hint.setTextSize(12);
        hint.setPadding(0, 0, 0, 14);
        root.addView(hint);

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(0xFF060809);
        TextView body = new TextView(this);
        body.setText(report);
        body.setTypeface(Typeface.MONOSPACE);
        body.setTextSize(10);
        body.setTextColor(0xFFD7E2EE);
        body.setTextIsSelectable(true);
        body.setPadding(14, 14, 14, 14);
        sv.addView(body);
        root.addView(sv, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, 14, 0, 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(4, 0, 4, 0);

        Button save = button("保存文件", 0xFF2D6AE0);
        Button retry = button("重试进入系统", 0xFF2E7D5B);
        Button close = button("关闭", 0xFF3A4450);
        row.addView(save, lp);
        row.addView(retry, lp);
        row.addView(close, lp);
        root.addView(row);

        save.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                ReportSaver.saveAndShare(LaunchActivity.this, report);
            }
        });
        retry.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { retryBoot(); }
        });
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { finish(); }
        });

        setContentView(root);
    }

    private void retryBoot() {
        // 报告已展示，清「未读」；清阶段与启动标记，让本次重试重新计时
        try { CrashHandler.pendingFile(this).delete(); } catch (Throwable ignored) {}
        CrashHandler.clearStage(this);
        CrashHandler.clearLaunchStart(this);
        CrashHandler.markLaunchStart(this);
        CrashHandler.markStage(this, "STAGE_RETRY_BOOT");
        BootTrace.mark("用户点「重试进入系统」");
        startActivity(new Intent(this, BootActivity.class));
        finish();
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
}
