package dev.aether.vm;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * Aether VM 启动诊断浮层（自研）。
 *
 * 黑屏时 SDL 画面可能是纯黑的，但 SDL 用的是普通 SurfaceView（未 setZOrderOnTop），
 * 所以叠在 DecorView 上的普通 View 一定能画出来 —— 屏幕不会再是纯黑无信息。
 *
 * 实时显示：启动阶段、引擎标准输出/报错、内核串口日志、logcat 关键行。
 * 【1.0.7】「保存文件」把完整报告写成 .txt 并弹出系统分享，用户把原文件
 * 发给开发者，不再经过剪贴板（粘贴会被长度截断）。
 * 「隐藏」折叠成右上角小按钮，确认系统画面正常后可全屏查看。
 */
public final class DiagnosticsOverlay
        implements UpdateManager.Callback, UpdateManager.UiBridge {

    private final Activity act;
    private final File vmDir;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private FrameLayout root;
    private LinearLayout panel;
    private Button mini;
    private TextView status;
    private TextView body;
    private volatile boolean expanded = true;
    private volatile String engineState = "引擎：等待 SDL 线程启动 …";
    private volatile String updateHint = "";
    private volatile boolean updateChecked = false;

    public DiagnosticsOverlay(Activity a, File vmDir) {
        this.act = a;
        this.vmDir = vmDir;
    }

    /** 引擎状态由 BootActivity 更新 */
    public void setEngineState(String s) {
        engineState = s;
        BootTrace.mark("[DIAG] " + s);
        refresh();
    }

    public void attach() {
        root = new FrameLayout(act);

        panel = new LinearLayout(act);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xE6101622);          // 半透明深蓝，黑屏上也看得见
        panel.setPadding(14, 14, 14, 12);

        TextView title = new TextView(act);
        title.setText("Aether VM · 启动诊断");
        title.setTextColor(0xFF7FD1FF);
        title.setTextSize(15);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        panel.addView(title);

        status = new TextView(act);
        status.setTextColor(0xFFE6EDF5);
        status.setTextSize(12);
        status.setPadding(0, 8, 0, 6);
        status.setTypeface(Typeface.MONOSPACE);
        panel.addView(status);

        ScrollView sv = new ScrollView(act);
        sv.setBackgroundColor(0xFF080B10);
        body = new TextView(act);
        body.setTextColor(0xFFB9C9DA);
        body.setTextSize(10);
        body.setTypeface(Typeface.MONOSPACE);
        body.setTextIsSelectable(true);
        body.setPadding(10, 8, 10, 8);
        sv.addView(body);
        panel.addView(sv, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, 10, 0, 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(3, 0, 3, 0);

        Button save = button("保存文件", 0xFF2456C8);
        Button hide = button("隐藏", 0xFF33404E);
        Button refreshBtn = button("刷新", 0xFF2E6B52);
        Button updateBtn = button("系统更新", 0xFF6A4BC8);
        row.addView(save, lp);
        row.addView(refreshBtn, lp);
        row.addView(updateBtn, lp);
        row.addView(hide, lp);
        panel.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        save.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                ReportSaver.saveAndShare(act, buildReport());
            }
        });
        refreshBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { refresh(); }
        });
        updateBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                act.startActivity(new android.content.Intent(act, UpdateActivity.class));
            }
        });
        hide.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { setExpanded(false); }
        });

        FrameLayout.LayoutParams pp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(300), Gravity.TOP);
        root.addView(panel, pp);

        mini = button("诊断", 0xCC2456C8);
        FrameLayout.LayoutParams mp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.RIGHT);
        mp.setMargins(0, dp(8), dp(8), 0);
        mini.setVisibility(View.GONE);
        mini.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { setExpanded(true); }
        });
        root.addView(mini, mp);

        act.addContentView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // 立刻先画一帧，之后每 2s 刷新；即使引擎把 SDLThread 占满，UI 线程照常工作
        refresh();
        ui.post(refreshTask);

        // 冷启动静默检查一次公开 Release，不阻塞启动、不弹窗，只在状态行给出提示
        if (!updateChecked) {
            updateChecked = true;
            UpdateManager.check(act, this, this);
        }
    }

    /* ---- UpdateManager.UiBridge / Callback：状态行只显示一行客观提示 ---- */
    @Override public void post(Runnable r) { ui.post(r); }
    @Override public boolean isAlive() { return !act.isFinishing(); }

    @Override public void onCheckStart() { updateHint = ""; }
    @Override public void onProgress(int percent, long done, long total) {}
    @Override public void onDownloadDone(File apk, String error) {}

    @Override public void onCheckDone(UpdateManager.Info in, String error) {
        if (in == null || error != null) {
            updateHint = "";
            return;
        }
        updateHint = in.newer
                ? "\n[更新] 发现新版本 " + in.tag + "，点「系统更新」下载安装"
                : "\n[更新] 当前已是最新版本 " + in.tag;
    }

    private final Runnable refreshTask = new Runnable() {
        @Override public void run() {
            refresh();
            ui.postDelayed(this, 2000);
        }
    };

    private void setExpanded(boolean e) {
        expanded = e;
        panel.setVisibility(e ? View.VISIBLE : View.GONE);
        mini.setVisibility(e ? View.GONE : View.VISIBLE);
        if (e) refresh();
    }

    private Button button(String text, int color) {
        Button b = new Button(act);
        b.setText(text);
        b.setTextColor(Color.WHITE);
        b.setTextSize(12);
        b.setAllCaps(false);
        b.setBackgroundColor(color);
        b.setPadding(6, 4, 6, 4);
        return b;
    }

    private int dp(int v) {
        return (int) (v * act.getResources().getDisplayMetrics().density + 0.5f);
    }

    /** 拼一份完整、可复制的诊断报告 */
    private String buildReport() {
        StringBuilder b = new StringBuilder();
        b.append("========== Aether VM 启动诊断报告 ==========\n");
        b.append("时间: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA)
                .format(new Date())).append("\n");
        b.append("设备: ").append(Build.MANUFACTURER).append(" / ").append(Build.MODEL)
         .append("  Android ").append(Build.VERSION.RELEASE)
         .append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        b.append("ABI : ").append(Arrays.toString(Build.SUPPORTED_ABIS)).append("\n");
        try {
            b.append("版本: ").append(act.getPackageManager()
                    .getPackageInfo(act.getPackageName(), 0).versionName).append("\n");
        } catch (Throwable ignored) {}
        b.append("状态: ").append(engineState).append("\n\n");

        b.append("---- 引擎 stdout/stderr（QEMU 报错在这里）----\n")
         .append(tail(new File(vmDir, "qemu_stdio.log"), 200000)).append("\n");
        b.append("---- 内核串口 serial.log（开机到哪一步）----\n")
         .append(tail(new File(vmDir, "serial.log"), 200000)).append("\n");
        b.append("---- logcat（含 native tombstone）----\n").append(logcat()).append("\n");
        b.append("---- 运行配置 vm.conf.final ----\n")
         .append(tail(new File(vmDir, "vm.conf.final"), 16384)).append("\n");
        b.append("---- nativeLibraryDir 库清单 ----\n").append(libList()).append("\n");
        b.append("---- 启动阶段 BootTrace ----\n").append(BootTrace.read()).append("\n");
        b.append("========== 报告结束：点击「保存文件」导出本文件并发送 ==========\n");
        return b.toString();
    }

    private void refresh() {
        if (body == null || status == null) return;
        final String state = engineState;
        final String shortLog = tail(new File(vmDir, "qemu_stdio.log"), 2600);
        final String serial = tail(new File(vmDir, "serial.log"), 1200);
        final String trace = tail(BootTrace.read(), 1800);

        // 内核串口有实质输出 = QEMU 真的把 guest 跑起来了，本次启动视为成功。
        // 清除阶段/启动标记，避免下次冷启动把「正常开机后用户退出」误判为崩溃。
        // 若进程在 QEMU native 里崩掉，这行永远执行不到，标记保留 → 冷启动必报。
        if (!serial.startsWith("(") && serial.trim().length() > 16) {
            CrashHandler.clearStage(act);
            CrashHandler.clearLaunchStart(act);
        }
        final String hint = updateHint;
        ui.post(new Runnable() {
            @Override public void run() {
                status.setText(state + hint);
                StringBuilder s = new StringBuilder();
                s.append("[引擎输出]\n").append(shortLog.isEmpty() ? "(暂无)\n" : shortLog).append("\n");
                s.append("[内核串口]\n").append(serial.isEmpty() ? "(暂无)\n" : serial).append("\n");
                s.append("[启动阶段]\n").append(trace).append("\n");
                s.append("提示：若长时间无内核串口输出，说明引擎未成功启动，\n")
                 .append("点击「保存文件」导出完整 .txt 报告并发送（内容不做截断）。\n");
                body.setText(s.toString());
            }
        });
    }

    private String libList() {
        StringBuilder b = new StringBuilder();
        try {
            File d = new File(act.getApplicationInfo().nativeLibraryDir);
            File[] fs = d.listFiles();
            if (fs == null) return "  (目录不可读)\n";
            Arrays.sort(fs);
            for (File f : fs) b.append("  ").append(f.getName())
                    .append("  ").append(f.length()).append(" B\n");
        } catch (Throwable t) {
            b.append("  读取失败: ").append(t).append('\n');
        }
        return b.toString();
    }

    private String logcat() {
        StringBuilder b = new StringBuilder();
        java.lang.Process p = null;
        BufferedReader br = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{
                    "logcat", "-d", "-b", "main,system,crash", "-t", "3000", "-v", "threadtime"});
            br = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8), 8192);
            String line;
            int bytes = 0;
            while ((line = br.readLine()) != null) {
                b.append(line).append('\n');
                bytes += line.length() + 1;
                if (bytes > 500000) { b.append("...(日志已达上限)\n"); break; }
            }
        } catch (Throwable t) {
            b.append("(logcat 不可用: ").append(t).append(")\n");
        } finally {
            if (br != null) try { br.close(); } catch (Throwable ignored) {}
            if (p != null) try { p.destroy(); } catch (Throwable ignored) {}
        }
        return b.toString();
    }

    private static String tail(String s, int max) {
        if (s.length() <= max) return s;
        return "…(前略)\n" + s.substring(s.length() - max);
    }

    private static String tail(File f, int max) {
        if (f == null || !f.exists()) return "(无文件)";
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            long skip = Math.max(0, f.length() - max);
            long n = in.skip(skip);
            if (n < skip) { in.close(); in = new FileInputStream(f); }
            java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int r;
            while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
            return new String(bos.toByteArray(), StandardCharsets.UTF_8);
        } catch (Throwable t) {
            return "(读取失败: " + t + ")";
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }

}
