package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 应用外壳（头部区块 + 页签 + 内容区）。
 * 从 MainActivity 抽出：主界面与无头测试服务共用同一套界面构建代码，保证渲染与实机一致。
 * 视觉口径（v5.1 头部重设计）：字标（Geist）+ 区域方点记号；控制台读数行（mono 左核心右方案）；
 * 下划线式页签；刷新降噪为图标。
 */
public class Shell {
    public LinearLayout root;
    public LinearLayout content;
    public View coreDot;
    public TextView coreLine;
    public TextView schemeLine;
    public final TextView[] tabLabels = new TextView[4];
    public final View[] pages = new View[4];
    public int tab = 0;

    public RegionsPage regionsPage;
    public OpsPage opsPage;
    public SchemesPage schemesPage;
    public StatusPage statusPage;

    /** 构建整个界面树（不调用 setContentView，调用方自行处理）。 */
    public void build(final Context ctx) {
        root = Ui.col(ctx);
        root.setBackgroundColor(Ui.BG);

        // ---- 头部区块（字标 + 控制台读数行 + 下划线页签）----
        LinearLayout header = Ui.col(ctx);
        header.setBackground(Ui.vGradient(0xFF14171B, 0xFF0C0E10));

        LinearLayout titleRow = Ui.row(ctx);
        titleRow.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 11), Ui.dp(ctx, 8), Ui.dp(ctx, 0));
        View mark = new View(ctx);
        mark.setBackground(Ui.brandMark(ctx));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(Ui.dp(ctx, 20), Ui.dp(ctx, 20));
        blp.rightMargin = Ui.dp(ctx, 9);
        mark.setLayoutParams(blp);
        titleRow.addView(mark);
        TextView title = Ui.tv(ctx, "vtouch", 16, Ui.TEXT, false);
        title.setTypeface(Ui.brandFont(ctx));
        title.setLetterSpacing(0.01f);
        titleRow.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView refresh = Ui.btnFlat(ctx, "⟳", Ui.DIM);
        refresh.setTextSize(15);
        refresh.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                refreshCurrent();
                Ui.toast("已刷新");
            }
        });
        titleRow.addView(refresh);
        header.addView(titleRow);

        LinearLayout statusRow = Ui.row(ctx);
        statusRow.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 6), Ui.dp(ctx, 16), Ui.dp(ctx, 3));
        coreDot = new View(ctx);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(Ui.dp(ctx, 8), Ui.dp(ctx, 8));
        dlp.rightMargin = Ui.dp(ctx, 7);
        coreDot.setLayoutParams(dlp);
        coreDot.setBackground(Ui.round(ctx, Ui.FAINT, 999));
        statusRow.addView(coreDot);
        coreLine = Ui.mono(ctx, "核心未运行", 11.5f, Ui.DIM);
        statusRow.addView(coreLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        schemeLine = Ui.mono(ctx, "方案 …", 11.5f, Ui.ACCENT);
        statusRow.addView(schemeLine);
        header.addView(statusRow);

        // 下划线式页签：选中 = 琥珀字 + 2dp 下划线
        LinearLayout tabRow = Ui.row(ctx);
        tabRow.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 2), Ui.dp(ctx, 12), Ui.dp(ctx, 4));
        String[] names = {"区域", "操作", "方案", "状态"};
        for (int i = 0; i < 4; i++) {
            final int idx = i;
            LinearLayout tabV = Ui.col(ctx);
            tabV.setGravity(Gravity.CENTER_HORIZONTAL);
            tabV.setMinimumHeight(Ui.dp(ctx, 48));
            tabV.setPadding(0, Ui.dp(ctx, 8), 0, Ui.dp(ctx, 8));
            tabV.setBackground(Ui.pressable(ctx, 0x00000000, 0x30FFFFFF, 10));
            tabV.setClickable(true);
            tabV.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { showTab(idx); }
            });
            TextView lab = Ui.tv(ctx, names[i], 14, Ui.DIM, false);
            lab.setGravity(Gravity.CENTER);
            lab.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 6), Ui.dp(ctx, 16), Ui.dp(ctx, 8));
            tabV.addView(lab, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
            tabRow.addView(tabV, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            tabLabels[i] = lab;
        }
        header.addView(tabRow);

        root.addView(header, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(Ui.divider(ctx));

        content = Ui.col(ctx);
        root.addView(content, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        regionsPage = new RegionsPage(ctx);
        opsPage = new OpsPage(ctx);
        schemesPage = new SchemesPage(ctx);
        statusPage = new StatusPage(ctx);
        pages[0] = regionsPage.build();
        pages[1] = opsPage.build();
        pages[2] = schemesPage.build();
        pages[3] = statusPage.build();
    }

    public void showTab(int i) {
        tab = i;
        content.removeAllViews();
        content.addView(pages[i]);
        for (int k = 0; k < 4; k++) {
            boolean sel = k == i;
            TextView lab = tabLabels[k];
            lab.setTextColor(sel ? Ui.ACCENT : Ui.DIM);
            lab.setTypeface(lab.getTypeface(), sel ? android.graphics.Typeface.BOLD
                    : android.graphics.Typeface.NORMAL);
            lab.setBackground(sel ? Ui.underlineDot(lab.getContext()) : null);
        }
        refreshCurrent();
    }

    /** 若当前页处于编辑态：先退出编辑并返回 true（返回键优先退编辑）。 */
    public boolean closeEditor() {
        if (tab == 0) return regionsPage.closeEditor();
        if (tab == 1) return opsPage.closeEditor();
        return false;
    }

    public void refreshCurrent() {
        refreshHeader();
        if (tab == 0) regionsPage.refresh();
        else if (tab == 1) opsPage.refresh();
        else if (tab == 2) schemesPage.refresh();
        else statusPage.refresh();
    }

    public void refreshHeader() {
        Ui.bg(new Runnable() {
            public void run() {
                final String pid = Su.corePid();
                final String cur = Schemes.current();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        boolean running = !pid.isEmpty();
                        coreLine.setText(running ? "核心运行中 pid " + pid : "核心未运行");
                        coreLine.setTextColor(running ? Ui.OK : Ui.DIM);
                        coreDot.setBackground(running
                                ? Ui.glowDot(coreDot.getContext(), Ui.OK)
                                : Ui.round(coreDot.getContext(), Ui.FAINT, 999));
                        schemeLine.setText("方案 " + (cur.isEmpty() ? "…" : cur));
                    }
                });
            }
        }, null);
    }
}
