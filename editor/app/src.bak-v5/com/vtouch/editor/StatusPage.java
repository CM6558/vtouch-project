package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** 状态页：核心进程状态 / 启停 / 自检 / 日志尾（输出进控制台面板；按钮随核心状态启用/禁用）。 */
public class StatusPage {
    private final Context act;
    private TextView out;
    private TextView startBtn, stopBtn;

    public StatusPage(Context a) { act = a; }

    public View build() {
        LinearLayout root = Ui.col(act);

        LinearLayout bar = Ui.row(act);
        bar.setPadding(Ui.dp(act, 16), Ui.dp(act, 10), Ui.dp(act, 8), Ui.dp(act, 6));
        TextView cap = Ui.tv(act, "核心控制", 12, Ui.DIM, false);
        bar.addView(cap, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        startBtn = Ui.btnSmallPrimary(act, "启动核心");
        startBtn.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { coreCmd("start"); }
        });
        stopBtn = Ui.btnFlat(act, "停止核心", Ui.WARN);
        stopBtn.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { coreCmd("stop"); }
        });
        TextView test = Ui.btnFlat(act, "自检", Ui.ACCENT);
        test.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { Ui.runSelfTestFlow(); }
        });
        TextView log = Ui.btnFlat(act, "日志尾", Ui.TEXT);
        log.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { logTail(); }
        });
        bar.addView(startBtn);
        bar.addView(stopBtn);
        bar.addView(test);
        bar.addView(log);
        root.addView(bar);

        ScrollView sc = new ScrollView(act);
        LinearLayout console = Ui.panel(act);
        console.setGravity(Gravity.TOP);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        clp.setMargins(Ui.dp(act, 16), Ui.dp(act, 8), Ui.dp(act, 16), Ui.dp(act, 16));
        console.setLayoutParams(clp);
        out = Ui.mono(act, "", 12, Ui.TEXT);
        out.setTextIsSelectable(true);
        console.addView(out);
        sc.addView(console);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    /** 按核心运行状态启用/禁用启动/停止按钮。 */
    private void applyCoreState(boolean running) {
        startBtn.setEnabled(!running);
        startBtn.setTextColor(running ? 0xFF5A6473 : 0xFFFFFFFF);
        startBtn.setBackground(Ui.pressable(act, running ? Ui.CARD2 : Ui.ACCENT,
                running ? 0xFF2E394C : 0xFF3A78E8, 10));
        stopBtn.setEnabled(running);
        stopBtn.setTextColor(running ? Ui.WARN : 0xFF5A6473);
    }

    public void refresh() {
        Ui.bg(new Runnable() {
            public void run() {
                Su.R id = Su.run("id");
                boolean root = id.ok() && id.out.contains("uid=0");
                String pid = Su.corePid();
                Su.R ls = Su.run("cd /data/local/vtouch-runtime 2>/dev/null && ls -l regions.conf ops.conf current; echo; cd /data/local/tmp && ls -l vtouchd_ui 2>&1");
                final android.text.SpannableStringBuilder b = new android.text.SpannableStringBuilder();
                appendColored(b, pid.isEmpty() ? "● 核心 未运行" : ("● 核心 运行中 pid " + pid),
                        pid.isEmpty() ? Ui.DIM : Ui.OK);
                b.append('\n');
                appendColored(b, root ? "● su root 可用" : ("● su 不可用 " + id.trimmed()),
                        root ? Ui.OK : Ui.WARN);
                b.append("\n\n");
                b.append(ls.out).append('\n');
                final boolean running = !pid.isEmpty();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        out.setText(b);
                        applyCoreState(running);
                    }
                });
            }
        }, null);
    }

    /** 往 SpannableStringBuilder 追加一段着色文本。 */
    private static void appendColored(android.text.SpannableStringBuilder b, String s, int color) {
        int st = b.length();
        b.append(s);
        b.setSpan(new android.text.style.ForegroundColorSpan(color), st, b.length(),
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }

    /** 只刷新按钮状态（不覆盖控制台输出）。 */
    private void updateCoreState() {
        Ui.bg(new Runnable() {
            public void run() {
                final String pid = Su.corePid();
                Ui.MAIN.post(new Runnable() {
                    public void run() { applyCoreState(!pid.isEmpty()); }
                });
            }
        }, null);
    }

    private void coreCmd(final String what) {
        Ui.bg(new Runnable() {
            public void run() {
                final Su.R r = Su.run("sh /sdcard/vtouch.sh " + what);
                final String o = r.out;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        Ui.toast(r.ok() ? ("核心 " + what + " 完成") : ("失败：" + r.trimmed()));
                        out.setText("$ vtouch.sh " + what + "\n" + o);
                        Ui.refreshHeader();
                        updateCoreState();
                    }
                });
            }
        }, null);
    }

    private void logTail() {
        Ui.bg(new Runnable() {
            public void run() {
                Su.R r = Su.run("for f in /data/local/tmp/vt_ui_core.log /data/local/tmp/vtouchd.log /data/local/vtouch-runtime/log.txt; do"
                        + " if [ -f \"$f\" ]; then echo \"== $f ==\"; tail -n 60 \"$f\"; fi; done;"
                        + " echo \"== /data/local/tmp ==\"; ls -l /data/local/tmp/ 2>&1");
                final String text = r.out.isEmpty() ? "（无输出）" : r.out;
                Ui.MAIN.post(new Runnable() {
                    public void run() { out.setText(text); }
                });
            }
        }, null);
    }

    void showReport(String report) {
        out.setText(report);
    }
}
