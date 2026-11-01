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
    public android.widget.FrameLayout root;
    public LinearLayout content;
    public View coreDot;
    public TextView coreLine;
    public TextView schemeLine;
    public final TextView[] tabLabels = new TextView[5];
    public final View[] pages = new View[5];
    public int tab = 0;

    public RegionsPage regionsPage;
    public OpsPage opsPage;
    public SchemesPage schemesPage;
    public AssetPage assetPage;
    public StatusPage statusPage;

    /** 构建整个界面树（不调用 setContentView，调用方自行处理）。 */
    public void build(final Context ctx) {
        root = new android.widget.FrameLayout(ctx);
        root.setBackgroundColor(Ui.BG);
        LinearLayout main = Ui.col(ctx);
        root.addView(main, new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT));

        // ---- 头部区块（字标 + 控制台读数行 + 下划线页签）----
        LinearLayout header = Ui.col(ctx);
        header.setBackground(Ui.vGradient(0xFF14171B, 0xFF0C0E10));

        LinearLayout titleRow = Ui.row(ctx);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        titleRow.setPadding(Ui.dp(ctx, 10), Ui.dp(ctx, 8), Ui.dp(ctx, 8), Ui.dp(ctx, 0));
        // 菜单按钮（☰）：打开侧边栏
        View menuBtn = new View(ctx);
        menuBtn.setBackground(new MenuDrawable(ctx));
        menuBtn.setOnTouchListener(new View.OnTouchListener() {
            public boolean onTouch(View v, android.view.MotionEvent e) {
                int a = e.getActionMasked();
                if (a == android.view.MotionEvent.ACTION_DOWN) v.setAlpha(0.6f);
                else if (a == android.view.MotionEvent.ACTION_UP
                        || a == android.view.MotionEvent.ACTION_CANCEL) v.setAlpha(1f);
                return false;
            }
        });
        menuBtn.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { openSidebar(); }
        });
        LinearLayout.LayoutParams mbLp = new LinearLayout.LayoutParams(Ui.dp(ctx, 40), Ui.dp(ctx, 40));
        mbLp.rightMargin = 0;
        titleRow.addView(menuBtn, 0, mbLp);
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
        String[] names = {"区域", "操作", "方案", "素材", "状态"};
        for (int i = 0; i < 5; i++) {
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

        main.addView(header, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        main.addView(Ui.divider(ctx));

        content = Ui.col(ctx);
        contentWrap = new android.widget.FrameLayout(ctx);
        contentWrap.addView(content, new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT));
        main.addView(contentWrap, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        regionsPage = new RegionsPage(ctx);
        opsPage = new OpsPage(ctx);
        schemesPage = new SchemesPage(ctx);
        assetPage = new AssetPage(ctx);
        statusPage = new StatusPage(ctx);
        pages[0] = regionsPage.build();
        pages[1] = opsPage.build();
        pages[2] = schemesPage.build();
        pages[3] = assetPage.build();
        pages[4] = statusPage.build();

        // ---- 侧边栏：全屏层（遮罩 + 左滑抽屉；☰ 在标题栏）----
        sbScrim = new View(ctx);
        sbScrim.setBackgroundColor(0x66000000);
        sbScrim.setVisibility(View.GONE);
        sbScrim.setClickable(true);
        sbScrim.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { closeSidebar(); }
        });
        root.addView(sbScrim, new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT));

        sbDrawer = Ui.col(ctx);
        sbDrawer.setBackgroundColor(0xFF14171A);
        sbDrawer.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 10), Ui.dp(ctx, 16), Ui.dp(ctx, 10));
        sbDrawer.setVisibility(View.GONE);
        LinearLayout sbHead = Ui.row(ctx);
        sbHead.setGravity(Gravity.CENTER_VERTICAL);
        sbHead.addView(Ui.tv(ctx, "侧边栏", 15, Ui.TEXT, true),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView sbClose = Ui.btnFlat(ctx, "\u2715", Ui.DIM);
        sbClose.setMinWidth(Ui.dp(ctx, 48));
        sbClose.setMinHeight(Ui.dp(ctx, 48));
        sbClose.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { closeSidebar(); }
        });
        sbHead.addView(sbClose);
        sbDrawer.addView(sbHead);
        sbDrawer.addView(Ui.divider(ctx));
        sbDrawer.addView(Ui.label(ctx, "编辑悬浮窗"));
        sbSwitch = Forms.switchRow(ctx, "浮标（编辑工具窗口）",
                "开：屏幕上出现品牌浮标，点它打开工具面板",
                CaptureChip.isRunning(), new android.widget.CompoundButton.OnCheckedChangeListener() {
                    public void onCheckedChanged(android.widget.CompoundButton b, boolean on) {
                        if (sbSyncing) return;
                        if (on) {
                            CaptureChip.startPanel(ctx);
                            Ui.toast("浮标已开启");
                        } else {
                            CaptureChip.stop(ctx);
                            Ui.toast("浮标已关闭");
                        }
                        final TextView st = sbState;
                        if (st != null) {
                            st.postDelayed(new Runnable() {
                                public void run() { refreshSidebar(); }
                            }, 800);
                        }
                    }
                });
        sbDrawer.addView(sbSwitch.row);
        sbState = Ui.mono(ctx, "", 11, Ui.DIM);
        sbState.setPadding(0, Ui.dp(ctx, 2), 0, Ui.dp(ctx, 8));
        sbDrawer.addView(sbState);
        sbDrawer.addView(Ui.divider(ctx));
        TextView sbHelp = Ui.tv(ctx,
                "用法：浮标单击=抓帧/开面板；拖动=移位（松手吸附近边收起）；"
                        + "收起把手=点一下展开；把手长按=完全退出。",
                11, Ui.FAINT, false);
        sbHelp.setLineSpacing(Ui.dp(ctx, 3), 1f);
        sbHelp.setPadding(0, Ui.dp(ctx, 8), 0, 0);
        sbDrawer.addView(sbHelp);
        android.widget.FrameLayout.LayoutParams dLp = new android.widget.FrameLayout.LayoutParams(
                Ui.dp(ctx, 300), android.widget.FrameLayout.LayoutParams.MATCH_PARENT);
        dLp.gravity = Gravity.LEFT;
        root.addView(sbDrawer, dLp);
    }

    private void openSidebar() {
        if (sbDrawer == null || sbOpen) return;
        sbOpen = true;
        refreshSidebar();
        sbScrim.setVisibility(View.VISIBLE);
        sbScrim.setAlpha(0f);
        sbScrim.animate().alpha(1f).setDuration(180).start();
        sbDrawer.setVisibility(View.VISIBLE);
        sbDrawer.setTranslationX(-Ui.dp(sbDrawer.getContext(), 300));
        sbDrawer.animate().translationX(0f).setDuration(180).start();
    }

    private void closeSidebar() {
        if (sbDrawer == null || !sbOpen) return;
        sbOpen = false;
        sbDrawer.animate().translationX(-Ui.dp(sbDrawer.getContext(), 300)).setDuration(160)
                .withEndAction(new Runnable() {
                    public void run() { if (!sbOpen) sbDrawer.setVisibility(View.GONE); }
                }).start();
        sbScrim.animate().alpha(0f).setDuration(160).withEndAction(new Runnable() {
            public void run() { if (!sbOpen) sbScrim.setVisibility(View.GONE); }
        }).start();
    }

    private void refreshSidebar() {
        if (sbSwitch == null) return;
        boolean on = CaptureChip.isRunning();
        sbSyncing = true;
        if (sbSwitch.sw.isChecked() != on) sbSwitch.sw.setChecked(on);
        sbSyncing = false;
        if (sbState != null) {
            sbState.setText(on ? "状态：浮标在屏（拖到边缘可收起）" : "状态：已关闭");
        }
    }

    public void showTab(int i) {
        tab = i;
        content.removeAllViews();
        content.addView(pages[i]);
        for (int k = 0; k < 5; k++) {
            boolean sel = k == i;
            TextView lab = tabLabels[k];
            lab.setTextColor(sel ? Ui.ACCENT : Ui.DIM);
            lab.setTypeface(lab.getTypeface(), sel ? android.graphics.Typeface.BOLD
                    : android.graphics.Typeface.NORMAL);
            lab.setBackground(sel ? Ui.underlineDot(lab.getContext()) : null);
        }
        refreshCurrent();
    }

    public android.widget.FrameLayout contentWrap;
    public View layerView;
    public Runnable layerBack;

    // ---- 侧边栏（左缘把手 → 抽屉：悬浮窗开关）----
    private View sbScrim;
    private LinearLayout sbDrawer;
    private Forms.SwitchRow sbSwitch;
    private TextView sbState;
    private boolean sbOpen = false;
    private boolean sbSyncing = false;

    /** 打开全屏叠加层（内容区；看图器会话等）。onBack = 返回键处理（一般为会话取消）。 */
    public void openLayer(View v, Runnable onBack) {
        closeLayer();
        layerBack = onBack;
        layerView = v;
        contentWrap.addView(v, new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT));
    }

    public void closeLayer() {
        if (layerView != null && contentWrap != null) contentWrap.removeView(layerView);
        layerView = null;
        layerBack = null;
    }

    public boolean layerOpen() { return layerView != null; }

    /** 若当前页处于编辑态：先退出编辑并返回 true（叠层 > 页面编辑，返回键优先）。 */
    public boolean closeEditor() {
        if (layerView != null) {
            Runnable b = layerBack;
            if (b != null) b.run(); else closeLayer();
            return true;
        }
        if (tab == 0) return regionsPage.closeEditor();
        if (tab == 1) return opsPage.closeEditor();
        return false;
    }

    public void refreshCurrent() {
        refreshHeader();
        if (tab == 0) regionsPage.refresh();
        else if (tab == 1) opsPage.refresh();
        else if (tab == 2) schemesPage.refresh();
        else if (tab == 3) assetPage.refresh();
        else statusPage.refresh();
    }

    /** 菜单记号（☰）：三条圆角横线，居中自适应。 */
    private static final class MenuDrawable extends android.graphics.drawable.Drawable {
        private final android.graphics.Paint p =
                new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final float lineW, lineH, gap;

        MenuDrawable(Context c) {
            lineW = Ui.dp(c, 15);
            lineH = Ui.dp(c, 2f);
            gap = Ui.dp(c, 4.4f);
            p.setColor(Ui.TEXT);
        }

        @Override public void draw(android.graphics.Canvas canvas) {
            android.graphics.Rect b = getBounds();
            float cx = b.exactCenterX();
            float cy = b.exactCenterY();
            float half = lineW / 2f;
            float y = cy - (lineH * 1.5f + gap) ;
            for (int i = 0; i < 3; i++) {
                canvas.drawRoundRect(cx - half, y, cx + half, y + lineH, lineH / 2f, lineH / 2f, p);
                y += lineH + gap;
            }
        }

        @Override public void setAlpha(int a) { p.setAlpha(a); }
        @Override public void setColorFilter(android.graphics.ColorFilter cf) { p.setColorFilter(cf); }
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
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
