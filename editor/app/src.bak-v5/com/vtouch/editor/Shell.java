package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 应用外壳（头部区块 + 页签 + 内容区）。
 * 从 MainActivity 抽出：主界面与无头测试服务共用同一套界面构建代码，保证渲染与实机一致。
 * 视觉口径（2026-10-07 美化版）：渐变头部 + 徽标；状态两枚胶囊（核心 / 方案）；
 * M3 胶囊式页签（选中 = 强调色 20% 底 + 强调色加粗字）。
 */
public class Shell {
    public LinearLayout root;
    public LinearLayout content;
    public TextView statusChip;
    public TextView schemeChip;
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

        // ---- 头部区块（渐层 + 徽标 + 状态胶囊 + 胶囊页签）----
        LinearLayout header = Ui.col(ctx);
        header.setBackground(Ui.vGradient(0xFF17212F, 0xFF111826));

        LinearLayout titleRow = Ui.row(ctx);
        titleRow.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 12), Ui.dp(ctx, 8), Ui.dp(ctx, 0));
        TextView badge = Ui.tv(ctx, "V", 13, 0xFFFFFFFF, true);
        badge.setGravity(Gravity.CENTER);
        badge.setBackground(Ui.round(ctx, Ui.ACCENT, 8));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(Ui.dp(ctx, 26), Ui.dp(ctx, 26));
        blp.rightMargin = Ui.dp(ctx, 10);
        badge.setLayoutParams(blp);
        titleRow.addView(badge);
        TextView title = Ui.tv(ctx, "vtouch 编辑台", 20, Ui.TEXT, true);
        titleRow.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView refresh = Ui.btnFlat(ctx, "⟳ 刷新", Ui.ACCENT);
        refresh.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                refreshCurrent();
                Ui.toast("已刷新");
            }
        });
        titleRow.addView(refresh);
        header.addView(titleRow);

        LinearLayout statusRow = Ui.row(ctx);
        statusRow.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 8), Ui.dp(ctx, 16), Ui.dp(ctx, 4));
        statusChip = Ui.chip(ctx, "● 核心未运行", Ui.DIM);
        schemeChip = Ui.chip(ctx, "方案 …", Ui.ACCENT);
        statusRow.addView(statusChip);
        statusRow.addView(schemeChip);
        header.addView(statusRow);

        // M3 胶囊页签：文字 + 选中胶囊底（替代旧下划线指示器）
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
            lab.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 6), Ui.dp(ctx, 16), Ui.dp(ctx, 6));
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
            lab.setBackground(sel ? Ui.round(lab.getContext(), 0x335C93FF, 14) : null);
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
                        statusChip.setText(running ? "● 核心运行中 pid " + pid : "● 核心未运行");
                        Ui.chipTint(statusChip, running ? Ui.OK : Ui.TEXT);
                        schemeChip.setText("方案 " + (cur.isEmpty() ? "…" : cur));
                        Ui.chipTint(schemeChip, Ui.ACCENT);
                    }
                });
            }
        }, null);
    }
}
