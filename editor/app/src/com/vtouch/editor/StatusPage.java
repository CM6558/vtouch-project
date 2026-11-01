package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** 状态页：Bento 四瓦片（核心控制 / su / 方案 / 工具）+ 全宽控制台（输出 / 日志 / 自检报告）。 */
public class StatusPage {
    private final Context act;
    private TextView out;
    private TextView startBtn;
    private TextView coreState, suState, schemeName, schemeMeta;
    private boolean coreBusy;
    private boolean coreRunning;

    public StatusPage(Context a) { act = a; }

    /** 瓦片：与卡片同款描边面，LP 由调用方设置（2×2 网格）。 */
    private LinearLayout tile() {
        LinearLayout t = Ui.col(act);
        t.setBackground(Ui.strokedRound(act, Ui.CARD, Ui.CARD_LINE, 8));
        int p = Ui.dp(act, 12);
        t.setPadding(p, p, p, p);
        return t;
    }

    private LinearLayout.LayoutParams halfLp(boolean left, int topDp, boolean stretch) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                stretch ? LinearLayout.LayoutParams.MATCH_PARENT : LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.leftMargin = Ui.dp(act, left ? 8 : 3);
        lp.rightMargin = Ui.dp(act, left ? 3 : 8);
        lp.topMargin = Ui.dp(act, topDp);
        return lp;
    }

    public View build() {
        LinearLayout root = Ui.col(act);

        // ---- Bento 2×2 ----
        LinearLayout row1 = Ui.row(act);
        row1.setGravity(Gravity.TOP);
        LinearLayout core = tile();
        core.addView(Ui.tv(act, "核心控制", 12, Ui.DIM, true));
        coreState = Ui.tv(act, "● 核心 未运行", 13, Ui.DIM, false);
        coreState.setPadding(0, Ui.dp(act, 4), 0, Ui.dp(act, 4));
        core.addView(coreState);
        startBtn = Ui.btnSmallPrimary(act, "启动核心");
        startBtn.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { coreCmd(coreRunning ? "stop" : "start"); }
        });
        core.addView(startBtn);
        core.addView(Ui.tv(act, "加载确认见控制台；旧核心仅显示进程状态", 11, Ui.DIM, false));
        row1.addView(core, halfLp(true, 6, false));

        LinearLayout su = tile();
        su.addView(Ui.tv(act, "su", 12, Ui.DIM, true));
        suState = Ui.tv(act, "● su …", 13, Ui.DIM, false);
        suState.setPadding(0, Ui.dp(act, 4), 0, 0);
        su.addView(suState);
        TextView suPath = Ui.tv(act, "读写 /data/local/vtouch-runtime", 11, Ui.DIM, false);
        suPath.setPadding(0, Ui.dp(act, 4), 0, 0);
        su.addView(suPath);
        row1.addView(su, halfLp(false, 6, true));

        LinearLayout row2 = Ui.row(act);
        row2.setGravity(Gravity.TOP);
        LinearLayout sch = tile();
        sch.addView(Ui.tv(act, "方案", 12, Ui.DIM, true));
        schemeName = Ui.monoBold(act, "…", 15, Ui.ACCENT);
        schemeName.setPadding(0, Ui.dp(act, 4), 0, 0);
        sch.addView(schemeName);
        schemeMeta = Ui.mono(act, "", 11, Ui.DIM);
        schemeMeta.setPadding(0, Ui.dp(act, 2), 0, 0);
        sch.addView(schemeMeta);
        row2.addView(sch, halfLp(true, 3, true));

        LinearLayout tools = tile();
        tools.addView(Ui.tv(act, "工具", 12, Ui.DIM, true));
        LinearLayout toolBtns = Ui.row(act);
        toolBtns.setPadding(0, Ui.dp(act, 4), 0, 0);
        TextView test = Ui.btnFlat(act, "自检", Ui.ACCENT);
        test.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { Ui.runSelfTestFlow(); }
        });
        TextView log = Ui.btnFlat(act, "日志尾", Ui.TEXT);
        log.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { logTail(); }
        });
        toolBtns.addView(test);
        toolBtns.addView(log);
        tools.addView(toolBtns);
        TextView recover = Ui.btnFlat(act, "恢复未完成发布", Ui.WARN);
        recover.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                final Ui.Dlg d = Ui.dialog(act, "恢复未完成发布？");
                d.body.addView(Ui.dlgMessage(act, "请先停止核心并确认没有其他窗口正在保存。只回滚 .publish-lock 日志记录的文件，不改普通方案备份。"));
                TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
                cancel.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { d.dismiss(); } });
                TextView confirm = Ui.btnSmallPrimary(act, "确认恢复");
                confirm.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        d.dismiss();
                        Ui.bg(new Runnable() {
                            public void run() {
                                final Su.R r = Su.corePid().isEmpty() ? Su.recoverPublish(OpsV8.base())
                                        : new Su.R(-1, "核心仍在运行，请先停止后恢复");
                                Ui.MAIN.post(new Runnable() {
                                    public void run() { out.setText(r.trimmed()); Ui.toast(r.ok() ? "已恢复" : "恢复未完成"); }
                                });
                            }
                        }, null);
                    }
                });
                d.actions.addView(cancel); d.actions.addView(confirm); d.show();
            }
        });
        tools.addView(recover);
        row2.addView(tools, halfLp(false, 3, false));

        // ---- 全宽控制台（横屏矮屏：整页一起滚，瓦片不被纵向挤压） ----
        boolean landscape = act.getResources().getDisplayMetrics().widthPixels
                > act.getResources().getDisplayMetrics().heightPixels;
        if (landscape) {
            ScrollView sc = new ScrollView(act);
            LinearLayout inner = Ui.col(act);
            inner.addView(row1);
            inner.addView(row2);
            inner.addView(consolePanel());
            sc.addView(inner);
            root.addView(sc, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        } else {
            root.addView(row1);
            root.addView(row2);
            ScrollView sc = new ScrollView(act);
            sc.addView(consolePanel());
            root.addView(sc, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        }
        return root;
    }

    /** 控制台面板（输出文本；横屏与竖屏共用）。 */
    private LinearLayout consolePanel() {
        LinearLayout console = Ui.panel(act);
        console.setGravity(Gravity.TOP);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        clp.setMargins(Ui.dp(act, 16), Ui.dp(act, 8), Ui.dp(act, 16), Ui.dp(act, 16));
        console.setLayoutParams(clp);
        out = Ui.mono(act, "", 12, Ui.TEXT);
        out.setTextIsSelectable(true);
        console.addView(out);
        return console;
    }

    /** 按核心运行状态启用/禁用启动/停止按钮。 */
    private void applyCoreState(boolean running) {
        coreRunning = running;
        startBtn.setText(coreBusy ? "处理中…" : running ? "停止核心" : "启动核心");
        startBtn.setEnabled(!coreBusy);
        startBtn.setTextColor(coreBusy ? Ui.FAINT : running ? Ui.WARN : Ui.ON_ACCENT);
        startBtn.setBackground(Ui.pressable(act, coreBusy || running ? Ui.CARD2 : Ui.ACCENT,
                coreBusy || running ? 0xFF2A3138 : Ui.ACCENT_PRESS, 6));
    }

    public void refresh() {
        Ui.bg(new Runnable() {
            public void run() {
                Su.R id = Su.run("id");
                boolean root = id.ok() && id.out.contains("uid=0");
                String pid = Su.corePid();
                Su.R ls = Su.run("cd /data/local/vtouch-runtime 2>/dev/null && ls -l regions.conf ops.conf current; echo; cd /data/local/tmp && ls -l vtouchd_ui 2>&1");
                String cur = Schemes.current();
                String cnt = Schemes.counts().get(cur);
                final String loadStatus = RuntimeStatus.read();
                final String suNote = root ? "" : "su 不可用：" + id.trimmed() + "\n";
                final String lsOut = ls.out.isEmpty() ? "（无输出）" : ls.out;
                final String curF = (cur == null || cur.isEmpty()) ? "…" : cur;
                final String cntF = cnt == null ? "" : cnt;
                final String pidF = pid;
                final boolean rootF = root;
                final boolean running = !pid.isEmpty();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        coreState.setText(running ? "● 进程存在 pid " + pidF : "● 核心 未运行");
                        coreState.setTextColor(running ? Ui.OK : Ui.DIM);
                        suState.setText(rootF ? "● su root 可用" : "● su 不可用");
                        suState.setTextColor(rootF ? Ui.OK : Ui.WARN);
                        schemeName.setText(curF);
                        schemeMeta.setText(cntF);
                        out.setText(suNote + loadStatus + "\n\n$ ls -l /data/local/vtouch-runtime\n" + lsOut);
                        applyCoreState(running);
                    }
                });
            }
        }, null);
    }

    /** 只刷新核心状态（瓦片 + 按钮），不覆盖控制台输出。 */
    private void updateCoreState() {
        Ui.bg(new Runnable() {
            public void run() {
                final String pid = Su.corePid();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        boolean running = !pid.isEmpty();
                        coreState.setText(running ? "● 进程存在 pid " + pid : "● 核心 未运行");
                        coreState.setTextColor(running ? Ui.OK : Ui.DIM);
                        applyCoreState(running);
                    }
                });
            }
        }, null);
    }

    private void coreCmd(final String what) {
        if (coreBusy) return;
        coreBusy = true;
        applyCoreState(coreRunning);
        Ui.bg(new Runnable() {
            public void run() {
                final Su.R r = Su.run("sh /sdcard/vtouch.sh " + what);
                final String o = r.out;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        coreBusy = false;
                        Ui.toast(r.ok() ? ("核心 " + what + " 完成") : ("失败：" + r.trimmed()));
                        out.setText("$ vtouch.sh " + what + "\n" + o);
                        applyCoreState(coreRunning);
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
