package com.vtouch.editor;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 看图器会话页（全屏叠层）：模板新建/编辑 + 查看 + 取点/吸色/点集（T7 补齐）+ 本地试一下。
 * 数据口径：对外读数 = 竖屏逻辑坐标（Matcher.frameToLogic）；素材写盘经 Assets（scratch 可重定向）。
 * 流程（模板）：[去抓帧]=武装浮标+退后台 → 抓完回跳自动刷新 → 框选/数字微调 → [保存并试一下]。
 */
public class VisualPage {
    public static final int MODE_TMPL_NEW = 1, MODE_TMPL_EDIT = 2, MODE_PICK_POINT = 3,
            MODE_PICK_COLOR = 4, MODE_VIEW = 5, MODE_PTS_NEW = 6, MODE_PTS_EDIT = 7,
            MODE_TMPL_TEST = 8, MODE_PTS_TEST = 9, MODE_REGION = 10, MODE_REGIONS = 11;   /* v12b：帧上单区 / v12c：全部区域总览 */

    public static class Session {
        public int mode;
        public String name, frameName, region;
        public int[] regionRect;
        public int[] regionGeom;          /* v12b 区域帧上编辑：预载几何（竖屏逻辑；矩形 [x1,y1,x2,y2] / 圆 [cx,cy,r]） */
        public boolean regionCircle;      /* v12b：圆形模式 */
        public int armNew;                /* v12d 总览：进入即置画具（0 关 / 1 矩形 / 2 圆，一次性） */
        public String focusId;            /* v12d 总览：进入即选中该 id（表单跳转用） */
        public int thresh = 8;
        public boolean colorCheck;
    }

    public interface Done {
        void onTmplSaved(String name);
        void onPointPicked(int lx, int ly);
        void onColorPicked(int rgb);
        void onCanceled();
        /** v12b 区域帧上编辑：确认时回传（竖屏逻辑坐标）。默认空实现（老实现不受影响）。 */
        default void onRegionPicked(int[] geom, boolean circle) { }
    }

    private static final class MiniNum {
        LinearLayout row;
        EditText et;
    }

    /** 限高滚动容器：底栏再长也不挤画面（v7.1）。 */
    private static final class MaxScroll extends android.widget.ScrollView {
        private final int maxH;
        MaxScroll(Context c, int maxH) { super(c); this.maxH = maxH; }
        @Override protected void onMeasure(int wSpec, int hSpec) {
            int h = android.view.View.MeasureSpec.getSize(hSpec);
            super.onMeasure(wSpec, android.view.View.MeasureSpec.makeMeasureSpec(Math.max(1, Math.min(h, maxH)),
                    android.view.View.MeasureSpec.AT_MOST));
        }
    }

    public static VisualPage current;
    public final LinearLayout root;

    private final Context ctx;
    private final Session s;
    private final Done done;

    private ImageView iv;
    private TextView titleT, frameLine, resultLine;
    private EditText nameE;
    private MiniNum mX, mY, mW, mH;
    private EditText threshE;
    private Forms.SwitchRow ccRow;
    private LinearLayout tmplBar;
    private android.widget.ScrollView barScroll;   /* 底栏滚动容器（v9.6 锁高；内容在栏内滚动，图区恒定） */

    private int frameW, frameH, frameRot;
    private byte[] frameRgba;
    private Matcher.Frame mfCache;
    private String curFrameName;
    private Assets.Tmpl editTmpl;
    private boolean reBox;                    // [重新截帧] 后恢复原框
    private boolean updatingNums;             // 双向同步守卫
    private boolean testing;                  // 试一下进行中
    private Matcher.Hit tmpHit;
    private int tmpTW, tmpTH;
    private String lastMsg = "";
    private int pickedFx = -1, pickedFy = -1;
    private TextView confirmB;
    private View swatch;
    private int ptsBaseX = -1, ptsBaseY = -1, ptsBaseRgb, ptsBaseTol = 8;
    private int reBaseX = -1, reBaseY = -1;
    private final List<int[]> ptsList = new ArrayList<int[]>();
    private Assets.Pts ptsPending;
    private boolean ptsPlaced;
    private LinearLayout ptsRows;
    private TextView ptsBaseLine;
    private byte[] testTmplGray;
    private int testTmplW, testTmplH, testTmplRot, testTmplAvg;
    private boolean testUseFile;
    /* v12c 全部区域总览调整 */
    private Regions.File regionsFile;
    private boolean saveDirty;
    private TextView regNewRectB, regNewCircB;    /* v12d 画具按钮（状态色） */

    public VisualPage(Context c, Session ss, Done d) {
        ctx = c;
        s = ss != null ? ss : new Session();
        done = d;
        root = build();
        String hint = hintText(s);                 /* v9.6：手势提示改开场 toast（原工具行整行让位给画面） */
        if (hint != null) Ui.toast(hint);
    }

    private static String hintText(Session ss) {
        int m = ss.mode;
        if (m == MODE_TMPL_NEW || m == MODE_TMPL_EDIT) return "单指拖 = 画框/调框 · 双指 = 平移/缩放";
        if (m == MODE_PTS_NEW || m == MODE_PTS_EDIT) return "点画面 = 取点（先取基准色）· 双指 = 平移/缩放";
        if (m == MODE_REGION) return ss.regionCircle ? "按下定圆心，拖动定半径 · 双指 = 平移/缩放"
                                                     : "单指拖 = 画框/调框 · 双指 = 平移/缩放";
        if (m == MODE_REGIONS) return "点选一个区域拖动/缩放 · 双指 = 平移/缩放";
        return null;
    }

    // ---------------------------------------------------------------- 打开/关闭

    public static VisualPage open(Session s, Done d) {
        MainActivity act = Ui.HOST;
        if (act == null) return null;
        VisualPage vp = new VisualPage(act, s, d);
        current = vp;
        act.shell.openLayer(vp.root, new Runnable() {
            public void run() { vp.close(); }
        });
        vp.attach(null);
        return vp;
    }

    public void close() {
        if (s.mode == MODE_REGIONS && saveDirty) { askAbandonRegions(); return; }
        closeInternal(true);
    }

    private void closeInternal(boolean canceled) {
        if (Ui.HOST != null && Ui.HOST.shell != null) Ui.HOST.shell.closeLayer();
        if (current == this) current = null;
        if (canceled && done != null) done.onCanceled();
    }

    /** 浮标抓帧回跳：把会话刷新到最新帧（编辑模式恢复原框）。 */
    public void useLatestFrame() { useLatestFrame(null); }

    /** 刷新到最新帧；after 在帧就绪后回调（悬浮取点用）。 */
    public void useLatestFrame(final Runnable after) {
        s.frameName = null;
        if (s.mode == MODE_TMPL_EDIT) {
            reBox = true;
            loadLatest(after);
        } else {
            attach(after);
        }
    }

    /** 悬浮屏幕取点回填（raster=抓帧像素坐标）：先换上那一刻的闪存帧，再按帧上点选同路径回填。 */
    public void screenPicked(final int rx, final int ry) {
        Capture.Shot fs = FrameStore.flashShot;
        boolean fresh = fs != null && System.currentTimeMillis() - FrameStore.flashTs < 30000;
        if (fresh || frameW <= 0 || frameRgba == null) {
            useLatestFrame(new Runnable() {
                public void run() { applyPick(rx, ry); }
            });
        } else {
            applyPick(rx, ry);
        }
    }

    private void applyPick(int rx, int ry) {
        if (frameW <= 0 || frameRgba == null) return;
        if (s.mode == MODE_PICK_POINT || s.mode == MODE_PICK_COLOR) {
            onIvPoint(clampI(rx, 0, Math.max(0, frameW - 1)), clampI(ry, 0, Math.max(0, frameH - 1)));
        } else {
            stashScreenPick(rx, ry);   /* 当前不是取点会话：暂存，等取点会话就绪再回填 */
        }
    }

    static int[] pendingScreenPick;
    static long pendingScreenPickTs;

    /** 冷启动等无会话场景：暂存屏幕取到的点，等下一次取点会话就绪时自动回填（60s 内有效）。 */
    public static void stashScreenPick(int x, int y) {
        pendingScreenPick = new int[]{x, y};
        pendingScreenPickTs = System.currentTimeMillis();
    }

    ImageView imageView() { return iv; }

    void setSelectionForTest(int x, int y, int w, int h) { iv.setSelection(x, y, w, h); }

    void setNameForTest(String n) { if (nameE != null) nameE.setText(n); }

    // ---------------------------------------------------------------- 构建

    private LinearLayout build() {
        LinearLayout r = Ui.col(ctx);
        r.setBackgroundColor(Ui.BG);
        // 顶栏
        LinearLayout top = Ui.row(ctx);
        top.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 8), Ui.dp(ctx, 8), Ui.dp(ctx, 6));
        TextView cancel = Ui.btnFlat(ctx, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { close(); }
        });
        top.addView(cancel);
        titleT = Ui.tv(ctx, titleFor(s.mode), 16, Ui.TEXT, true);
        top.addView(titleT, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        if (s.mode == MODE_TMPL_NEW || s.mode == MODE_TMPL_EDIT) {
            TextView save = Ui.btnSmallPrimary(ctx, "保存");
            save.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { onPrimary(); }
            });
            top.addView(save);
        }
        r.addView(top);
        // 帧源行
        LinearLayout fr = Ui.row(ctx);
        fr.setPadding(Ui.dp(ctx, 14), 0, Ui.dp(ctx, 8), Ui.dp(ctx, 4));
        frameLine = Ui.mono(ctx, "载入中…", 11.5f, Ui.DIM);
        frameLine.setSingleLine(true);
        frameLine.setEllipsize(android.text.TextUtils.TruncateAt.END);
        fr.addView(frameLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView pickFrame = Ui.btnSmall(ctx, "用帧 ▾");
        pickFrame.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { pickFrameDialog(); }
        });
        TextView goCap = Ui.btnSmall(ctx, "去抓帧");
        goCap.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { armCapture(); }
        });
        fr.addView(pickFrame);
        fr.addView(goCap);
        r.addView(fr);
        // 主区（v9.6：工具行并入——缩放悬浮在画面右下角，图区拿满剩余高度；手势提示见开场 toast）
        iv = new ImageView(ctx);
        iv.setListener(new ImageView.Listener() {
            public void onBox(int x, int y, int w, int h) {
                if (s.mode == MODE_REGION) updateRegionResult();   /* v12b：框选/拖改 → 逻辑读数 */
                else syncNumsFromBox();
            }
            public void onPoint(int x, int y) { onIvPoint(x, y); }
        });
        iv.setCircleListener(new ImageView.CircleListener() {
            public void onCircle(int cx, int cy, int r) {
                if (s.mode == MODE_REGION) updateRegionResult();
            }
        });
        iv.setRegionsListener(new ImageView.RegionsListener() {
            public void onRegionSelected(int idx) {
                if (s.mode == MODE_REGIONS) updateRegionsInfo();
            }
            public void onRegionGeometryChanged() {
                if (s.mode == MODE_REGIONS) { saveDirty = true; assignNewShapeIds(); updateRegionsInfo(); }
            }
        });
        android.widget.FrameLayout imgWrap = new android.widget.FrameLayout(ctx);
        imgWrap.addView(iv, new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT));
        LinearLayout zc = Ui.row(ctx);
        zc.setGravity(Gravity.CENTER_VERTICAL);
        android.graphics.drawable.GradientDrawable zbg = Ui.round(ctx, 0xF214171A, 10);
        zbg.setStroke(Ui.dp(ctx, 1), 0xFF323A43);
        zc.setBackground(zbg);
        zc.setPadding(Ui.dp(ctx, 3), Ui.dp(ctx, 3), Ui.dp(ctx, 3), Ui.dp(ctx, 3));
        TextView zo = chip("−");
        zo.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { iv.zoomStep(-1); }
        });
        TextView zf = chip("适应");
        zf.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { iv.fit(); }
        });
        TextView zi = chip("＋");
        zi.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { iv.zoomStep(1); }
        });
        zc.addView(zo);
        zc.addView(zf);
        zc.addView(zi);
        android.widget.FrameLayout.LayoutParams zcl = new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.WRAP_CONTENT,
                android.widget.FrameLayout.LayoutParams.WRAP_CONTENT);
        zcl.gravity = Gravity.BOTTOM | Gravity.END;
        zcl.bottomMargin = Ui.dp(ctx, 10);
        zcl.rightMargin = Ui.dp(ctx, 10);
        imgWrap.addView(zc, zcl);
        r.addView(imgWrap, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        // 底栏
        if (s.mode == MODE_TMPL_NEW || s.mode == MODE_TMPL_EDIT) {
            tmplBar = buildTmplBar();
            addBar(r, tmplBar, 0f);
        } else if (s.mode == MODE_PICK_POINT || s.mode == MODE_PICK_COLOR) {
            addBar(r, buildPickBar(), 0f);
        } else if (s.mode == MODE_PTS_NEW || s.mode == MODE_PTS_EDIT || s.mode == MODE_PTS_TEST) {
            addBar(r, buildPtsBar(), 0.30f);   /* 参考点列表要有窗口；锁高后加多少点图区都不缩 */
        } else if (s.mode == MODE_TMPL_TEST) {
            addBar(r, buildTmplTestBar(), 0f);
        } else if (s.mode == MODE_REGION) {          /* v12b：区域帧上编辑（矩形/圆） */
            addBar(r, buildRegionBar(), 0f);
        } else if (s.mode == MODE_REGIONS) {         /* v12c：全部区域总览调整 */
            addBar(r, buildRegionsBar(), 0f);
        }
        return r;
    }

    /** 底栏入列（v9.6 锁高：打开时按内容量一次自然高，取 [minFrac, 42% 屏高] 区间**定格**——
     *  之后内容增删只在栏内滚动，图区高度恒定不缩）。minFrac>0 供「参考点列表」类底栏留出窗口。 */
    private void addBar(LinearLayout r, LinearLayout bar, float minFrac) {
        int cap = (int) (ctx.getResources().getDisplayMetrics().heightPixels * 0.42f);
        int min = (int) (ctx.getResources().getDisplayMetrics().heightPixels * minFrac);
        int wpx = ctx.getResources().getDisplayMetrics().widthPixels;
        bar.measure(View.MeasureSpec.makeMeasureSpec(Math.max(1, wpx), View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        int fixed = clampI(bar.getMeasuredHeight(), min, cap);
        MaxScroll ms = new MaxScroll(ctx, cap);
        ms.setVerticalScrollBarEnabled(false);
        ms.addView(bar, new android.widget.ScrollView.LayoutParams(
                android.widget.ScrollView.LayoutParams.MATCH_PARENT,
                android.widget.ScrollView.LayoutParams.WRAP_CONTENT));
        r.addView(ms, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, fixed));
        barScroll = ms;
    }

    /** 底栏滚到顶（基准取完回看名称/基准行）。 */
    private void scrollBarToTop() {
        if (barScroll == null) return;
        barScroll.post(new Runnable() {
            public void run() { barScroll.smoothScrollTo(0, 0); }
        });
    }

    /** 底栏滚到参考点列表尾部（新增一行自动可见；post 等本帧布局完再算坐标）。 */
    private void scrollBarToRowsEnd() {
        if (barScroll == null || ptsRows == null) return;
        barScroll.post(new Runnable() {
            public void run() {
                int y = ptsRows.getBottom() + Ui.dp(ctx, 56) - barScroll.getHeight();
                barScroll.smoothScrollTo(0, Math.max(0, y));
            }
        });
    }

    /** 底栏滚到结果行（试查/提示更新后可见）。 */
    private void scrollBarToResult() {
        if (barScroll == null || resultLine == null) return;
        barScroll.post(new Runnable() {
            public void run() {
                int y = resultLine.getBottom() + Ui.dp(ctx, 16) - barScroll.getHeight();
                barScroll.smoothScrollTo(0, Math.max(0, y));
            }
        });
    }

    private String titleFor(int m) {
        if (m == MODE_TMPL_NEW) return "新建模板";
        if (m == MODE_TMPL_EDIT) return "编辑模板 " + (s.name == null ? "" : s.name);
        if (m == MODE_VIEW) return "查看帧";
        if (m == MODE_PICK_POINT) return "取点";
        if (m == MODE_PICK_COLOR) return "吸色";
        if (m == MODE_PTS_NEW) return "新建点集";
        if (m == MODE_PTS_EDIT) return "编辑点集 " + (s.name == null ? "" : s.name);
        if (m == MODE_TMPL_TEST) return "试一下 模板 " + (s.name == null ? "" : s.name);
        if (m == MODE_PTS_TEST) return "试一下 点集 " + (s.name == null ? "" : s.name);
        if (m == MODE_REGION) return s.regionCircle ? "框选区域（圆）" : "框选区域（矩形）";
        if (m == MODE_REGIONS) return "调整所有区域";
        return "点集";
    }

    private TextView tiny(String txt) {
        TextView t = Ui.tv(ctx, txt, 15, Ui.TEXT, false);
        t.setGravity(Gravity.CENTER);
        t.setBackground(Ui.pressable(ctx, Ui.CARD2, 0xFF2A3138, 6));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(Ui.dp(ctx, 44), Ui.dp(ctx, 36));
        lp.rightMargin = Ui.dp(ctx, 6);
        t.setLayoutParams(lp);
        t.setClickable(true);
        return t;
    }

    /** v9.6 悬浮缩放键（画面右下角小胶囊用；背景由父级胶囊提供，按压高亮内置）。 */
    private TextView chip(String txt) {
        TextView t = Ui.tv(ctx, txt, 14, Ui.TEXT, false);
        t.setGravity(Gravity.CENTER);
        t.setBackground(Ui.pressable(ctx, 0x00000000, 0x33FFFFFF, 6));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(Ui.dp(ctx, 42), Ui.dp(ctx, 32));
        t.setLayoutParams(lp);
        t.setClickable(true);
        return t;
    }

    private LinearLayout buildTmplBar() {
        LinearLayout bar = Ui.col(ctx);
        bar.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 4), Ui.dp(ctx, 12), Ui.dp(ctx, 8));
        // 名称 + （编辑模式）重新截帧
        LinearLayout nr = Ui.row(ctx);
        nr.addView(Ui.tv(ctx, "名称", 13, Ui.TEXT, false));
        nameE = Ui.textField(ctx, "如 tmpl1");
        nameE.setText(suggestName());
        LinearLayout.LayoutParams nl = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        nl.leftMargin = Ui.dp(ctx, 8);
        nr.addView(nameE, nl);
        if (s.mode == MODE_TMPL_EDIT) {
            TextView re = Ui.btnSmall(ctx, "重新截帧");
            re.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    reBox = true;
                    loadLatest(null);
                }
            });
            nr.addView(re);
        }
        bar.addView(nr);
        // 数字微调 x/y
        LinearLayout n1 = Ui.row(ctx);
        mX = miniNum(n1, "x", 0);
        mY = miniNum(n1, "y", 0);
        bar.addView(n1);
        LinearLayout n2 = Ui.row(ctx);
        mW = miniNum(n2, "宽", 0);
        mH = miniNum(n2, "高", 0);
        bar.addView(n2);
        // 阈值行 + 颜色校验
        LinearLayout tr = Ui.row(ctx);
        tr.setGravity(Gravity.CENTER_VERTICAL);
        tr.addView(Ui.tv(ctx, "试查阈值", 12, Ui.DIM, false));
        TextView tm10 = tiny("−10");
        TextView tm1 = tiny("−1");
        threshE = new EditText(ctx);
        threshE.setText(String.valueOf(s.thresh));
        threshE.setTextSize(13);
        threshE.setTextColor(Ui.TEXT);
        threshE.setInputType(InputType.TYPE_CLASS_NUMBER);
        threshE.setBackground(Ui.strokedRound(ctx, Ui.CARD2, Ui.CARD_LINE, 6));
        threshE.setPadding(Ui.dp(ctx, 6), Ui.dp(ctx, 4), Ui.dp(ctx, 6), Ui.dp(ctx, 4));
        LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(Ui.dp(ctx, 48), LinearLayout.LayoutParams.WRAP_CONTENT);
        tl.leftMargin = Ui.dp(ctx, 6);
        tl.rightMargin = Ui.dp(ctx, 6);
        tr.addView(threshE, tl);
        TextView tp1 = tiny("＋1");
        TextView tp10 = tiny("＋10");
        tr.addView(tm10);
        tr.addView(tm1);
        tr.addView(tp1);
        tr.addView(tp10);
        tm10.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(-10); } });
        tm1.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(-1); } });
        tp1.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(1); } });
        tp10.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(10); } });
        bar.addView(tr);
        ccRow = Forms.switchRow(ctx, "颜色校验", "命中区域平均色比对（每通道容差 48）", s.colorCheck, null);
        bar.addView(ccRow.row);
        // 结果行
        resultLine = Ui.mono(ctx, "", 12, Ui.DIM);
        resultLine.setSingleLine(false);
        bar.addView(resultLine);
        // 动作行
        LinearLayout ba = Ui.row(ctx);
        TextView test = Ui.btnSmall(ctx, "试一下");
        test.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runTest(); }
        });
        TextView st = Ui.btnSmallPrimary(ctx, "保存并试一下");
        st.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { onPrimary(); }
        });
        ba.addView(test);
        ba.addView(st);
        bar.addView(ba);
        return bar;
    }

    private LinearLayout buildPickBar() {
        LinearLayout bar = Ui.col(ctx);
        bar.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 6), Ui.dp(ctx, 12), Ui.dp(ctx, 8));
        resultLine = Ui.mono(ctx, s.mode == MODE_PICK_POINT
                ? "点画面取点（一次回填 x、y）" : "点画面取色（放大镜可精调）", 12, Ui.DIM);
        bar.addView(resultLine);
        LinearLayout ba = Ui.row(ctx);
        ba.setGravity(Gravity.CENTER_VERTICAL);
        if (s.mode == MODE_PICK_COLOR) {
            swatch = new View(ctx);
            LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(Ui.dp(ctx, 40), Ui.dp(ctx, 22));
            sl.rightMargin = Ui.dp(ctx, 10);
            swatch.setLayoutParams(sl);
            swatch.setBackground(Ui.strokedRound(ctx, Ui.CARD2, Ui.CARD_LINE, 4));
            ba.addView(swatch);
        }
        confirmB = Ui.btnSmallPrimary(ctx, "确认");
        confirmB.setEnabled(false);
        confirmB.setAlpha(0.62f);
        confirmB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { pickConfirm(); }
        });
        ba.addView(confirmB);
        bar.addView(ba);
        return bar;
    }

    /** v12b 区域帧上编辑底栏：逻辑读数（跟拖跟新）+ [重框] / [使用此框]。 */
    private LinearLayout buildRegionBar() {
        LinearLayout bar = Ui.col(ctx);
        bar.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 6), Ui.dp(ctx, 12), Ui.dp(ctx, 8));
        resultLine = Ui.mono(ctx, s.regionCircle ? "按下定圆心，拖动定半径（双指 = 平移/缩放）"
                                                 : "拖框选区域；框内拖动 = 挪框，四角 = 缩放（双指 = 平移/缩放）", 12, Ui.DIM);
        resultLine.setSingleLine(false);
        bar.addView(resultLine);
        LinearLayout ba = Ui.row(ctx);
        ba.setGravity(Gravity.CENTER_VERTICAL);
        TextView again = Ui.btnSmall(ctx, "重框");
        again.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                iv.clearSelection();
                iv.clearCircle();
                updateRegionResult();
            }
        });
        ba.addView(again);
        TextView use = Ui.btnSmallPrimary(ctx, s.regionCircle ? "使用此圆" : "使用此框");
        use.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { regionConfirm(); }
        });
        ba.addView(use);
        bar.addView(ba);
        return bar;
    }

    /** 逻辑读数：当前框/圆换算为竖屏逻辑坐标（拖改时实时跟新）。 */
    private void updateRegionResult() {
        if (resultLine == null || iv == null || frameRgba == null) return;
        if (s.regionCircle) {
            int[] c = iv.circle();
            if (c[2] < 8) {
                resultLine.setText("按下定圆心，拖动定半径（双指 = 平移/缩放）");
                resultLine.setTextColor(Ui.DIM);
                return;
            }
            int[] L = RegionDraw.circleToLogic(frameRot, frameW, frameH, c[0], c[1], c[2]);
            resultLine.setText("将用 圆 · 圆心 " + L[0] + "," + L[1] + " · r=" + L[2] + "（逻辑）");
        } else {
            int[] b = iv.selection();
            if (b[2] < 8 || b[3] < 8) {
                resultLine.setText("拖框选区域；框内拖动 = 挪框，四角 = 缩放（双指 = 平移/缩放）");
                resultLine.setTextColor(Ui.DIM);
                return;
            }
            int[] L = RegionDraw.rectToLogic(frameRot, frameW, frameH, b[0], b[1], b[2], b[3]);
            resultLine.setText("将用 矩形 · " + L[0] + "," + L[1] + " → " + L[2] + "," + L[3] + "（逻辑）");
        }
        resultLine.setTextColor(Ui.TEXT);
    }

    /** [使用此框]：帧上几何 → 竖屏逻辑 → Done 回填（保存仍走编辑页 [保存]）。 */
    void regionConfirm() {
        if (frameRgba == null) { Ui.toast("先选帧（[去抓帧] / [用帧]）"); return; }
        int[] geom;
        if (s.regionCircle) {
            int[] c = iv.circle();
            if (c[2] < 8) { Ui.toast("先在画面上画一个圆（按下定圆心，拖动定半径）"); return; }
            geom = RegionDraw.circleToLogic(frameRot, frameW, frameH, c[0], c[1], c[2]);
        } else {
            int[] b = iv.selection();
            if (b[2] < 8 || b[3] < 8) { Ui.toast("先在画面上框选区域（拖动即画框）"); return; }
            geom = RegionDraw.rectToLogic(frameRot, frameW, frameH, b[0], b[1], b[2], b[3]);
        }
        if (done != null) done.onRegionPicked(geom, s.regionCircle);
        closeInternal(false);
    }

    /* ---------------- v12c 全部区域总览调整 ---------------- */

    /** 总览底栏：读数（选中件逻辑坐标跟拖跟新）+ [还原] / [保存全部]。 */
    private LinearLayout buildRegionsBar() {
        LinearLayout bar = Ui.col(ctx);
        bar.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 6), Ui.dp(ctx, 12), Ui.dp(ctx, 8));
        resultLine = Ui.mono(ctx, "载入中…", 12, Ui.DIM);
        resultLine.setSingleLine(false);
        bar.addView(resultLine);
        LinearLayout ba = Ui.row(ctx);
        ba.setGravity(Gravity.CENTER_VERTICAL);
        regNewRectB = Ui.btnSmall(ctx, "＋ 矩形");
        regNewRectB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { iv.setNewArm(iv.newArm() == 1 ? 0 : 1); updateRegionsInfo(); }
        });
        ba.addView(regNewRectB);
        regNewCircB = Ui.btnSmall(ctx, "＋ 圆");
        regNewCircB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { iv.setNewArm(iv.newArm() == 2 ? 0 : 2); updateRegionsInfo(); }
        });
        ba.addView(regNewCircB);
        TextView revert = Ui.btnSmall(ctx, "还原");
        revert.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { reloadRegionsFromDisk(); }
        });
        ba.addView(revert);
        TextView saveAll = Ui.btnSmallPrimary(ctx, "保存全部");
        saveAll.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Ui.bg(new Runnable() {
                    public void run() {
                        String msg;
                        try { msg = saveAllRegionsSync(); }
                        catch (Throwable t) { msg = "保存失败：" + (t.getMessage() != null ? t.getMessage() : t); }
                        final String fm = msg;
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                Ui.toast(fm);
                                updateRegionsInfo();
                            }
                        });
                    }
                }, null);
            }
        });
        ba.addView(saveAll);
        bar.addView(ba);
        return bar;
    }

    /** 总览读数：画具提示 / 概览 / 选中件逻辑坐标（跟拖跟新；画具按钮随态着色）。 */
    private void updateRegionsInfo() {
        if (resultLine == null || iv == null) return;
        int arm = iv.newArm();
        if (regNewRectB != null) regNewRectB.setTextColor(arm == 1 ? Ui.ACCENT : Ui.TEXT);
        if (regNewCircB != null) regNewCircB.setTextColor(arm == 2 ? Ui.ACCENT : Ui.TEXT);
        String armHint = arm == 1 ? "拖动画面 = 画新矩形" : (arm == 2 ? "拖动画面 = 画新圆（按下定圆心，拖定半径）" : null);
        List<ImageView.RegShape> shs = iv.regions();
        if (shs.isEmpty()) {
            resultLine.setText(armHint != null ? armHint
                    : "还没有区域——用 [＋ 矩形] / [＋ 圆] 在画面上拖出第一个区域");
            resultLine.setTextColor(armHint != null ? Ui.TEXT : Ui.DIM);
            return;
        }
        int sel = iv.selectedRegion();
        if (sel < 0 || sel >= shs.size() || frameRgba == null) {
            resultLine.setText((armHint != null ? armHint + " · " : "")
                    + "共 " + shs.size() + " 个区域 · 点选一个开始调整（框内拖动=挪，角柄=缩放）");
            resultLine.setTextColor(Ui.TEXT);
            return;
        }
        ImageView.RegShape sh = shs.get(sel);
        String geom;
        if (sh.circle) {
            int[] L = RegionDraw.circleToLogic(frameRot, frameW, frameH, sh.g[0], sh.g[1], sh.g[2]);
            geom = "圆心 " + L[0] + "," + L[1] + " · r=" + L[2];
        } else {
            int[] L = RegionDraw.rectToLogic(frameRot, frameW, frameH, sh.g[0], sh.g[1], sh.g[2], sh.g[3]);
            geom = L[0] + "," + L[1] + " → " + L[2] + "," + L[3];
        }
        resultLine.setText("已选 " + sh.id + " · " + (sh.circle ? "圆" : "矩形") + " · " + geom + "（逻辑）"
                + (armHint != null ? " · " + armHint : ""));
        resultLine.setTextColor(Ui.TEXT);
    }

    /** v12d：给拖画新建的件命名（r 系取 r1·r2…、c 系取 c1·c2… 首个空闲：避开盘上已有与当前件表已占）。 */
    private void assignNewShapeIds() {
        if (iv == null) return;
        for (ImageView.RegShape sh : iv.regions()) {
            if (sh.id == null) sh.id = nextShapeId(sh.circle);
        }
    }

    private String nextShapeId(boolean circle) {
        for (int i = 1; i < 1000; i++) {
            String cand = (circle ? "c" : "r") + i;
            boolean used = regionsFile != null && regionsFile.find(cand) != null;
            if (!used) {
                for (ImageView.RegShape sh : iv.regions()) {
                    if (cand.equals(sh.id)) { used = true; break; }
                }
            }
            if (!used) return cand;
        }
        return (circle ? "c" : "r") + "999";
    }

    /** 由区域表（逻辑）重建当前帧的全部件（首次载入 / [还原] 共用）。 */
    private void rebuildShapesFromFile() {
        if (iv == null || regionsFile == null || frameRgba == null) return;
        ArrayList<ImageView.RegShape> shapes = new ArrayList<ImageView.RegShape>();
        for (Regions.Region r : regionsFile.regions) {
            ImageView.RegShape sh = new ImageView.RegShape();
            sh.id = r.id;
            sh.circle = r.type == 1;
            sh.en = r.en != 0;
            int[] lg = sh.circle ? new int[]{r.a1, r.a2, r.a3} : new int[]{r.a1, r.a2, r.a3, r.a4};
            projectLogic(sh, lg, frameRot, frameW, frameH);
            shapes.add(sh);
        }
        iv.setRegions(shapes);
    }

    /** 逻辑几何 → 帧件（rect lg=[x1,y1,x2,y2]；circle lg=[cx,cy,r]）。 */
    private void projectLogic(ImageView.RegShape sh, int[] lg, int rot, int w, int h) {
        if (sh.circle) {
            int[] fc = RegionDraw.circleToFrame(rot, w, h, lg[0], lg[1], lg[2]);
            sh.g[0] = fc[0]; sh.g[1] = fc[1]; sh.g[2] = fc[2]; sh.g[3] = 0;
        } else {
            int[] fr2 = Matcher.logicRectToFrame(rot, w, h, lg[0], lg[1], lg[2] - lg[0], lg[3] - lg[1]);
            sh.g[0] = fr2[0]; sh.g[1] = fr2[1]; sh.g[2] = fr2[2]; sh.g[3] = fr2[3];
        }
    }

    /** [还原]：重读盘上区域表并重建全部件（丢弃未保存调整）。 */
    private void reloadRegionsFromDisk() {
        final boolean wasDirty = saveDirty;
        Ui.bg(new Runnable() {
            public void run() {
                Regions.File f = null;
                String err = null;
                try { f = RegionDraw.loadLive(); }
                catch (Throwable t) { err = String.valueOf(t.getMessage() != null ? t.getMessage() : t); }
                final Regions.File ff = f;
                final String fe = err;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (fe != null) { Ui.toast("读取 regions.conf 失败：" + fe); return; }
                        regionsFile = ff;
                        rebuildShapesFromFile();
                        saveDirty = false;
                        updateRegionsInfo();
                        Ui.toast(wasDirty ? "已还原为盘上状态（未保存的调整已丢弃）" : "已从盘上重读区域");
                    }
                });
            }
        }, null);
    }

    /** [保存全部]：**只回写调整过的件**（帧件 → 竖屏逻辑；未动的保持盘上原值，避免无谓 ±1 漂移）；返回提示语。 */
    String saveAllRegionsSync() throws Exception {
        if (frameRgba == null) return "先选帧（[去抓帧] / [用帧]）";
        List<ImageView.RegShape> shs = iv.regions();
        if (shs.isEmpty()) return "没有区域可保存";
        ArrayList<ImageView.RegShape> movedSh = new ArrayList<ImageView.RegShape>();
        for (ImageView.RegShape sh : shs) if (sh.moved) movedSh.add(sh);
        if (movedSh.isEmpty()) return "没有改动（未动的区域保持原样）";
        Regions.File f = RegionDraw.loadLive();          /* 以盘上最新为基（bind/kind/其它行不碰） */
        int n = 0;
        for (ImageView.RegShape sh : movedSh) {
            if (sh.id == null) continue;                 /* 未命名（异常态）：跳过 */
            int[] g = sh.circle
                    ? RegionDraw.circleToLogic(frameRot, frameW, frameH, sh.g[0], sh.g[1], sh.g[2])
                    : RegionDraw.rectToLogic(frameRot, frameW, frameH, sh.g[0], sh.g[1], sh.g[2], sh.g[3]);
            Regions.Region r = f.find(sh.id);
            if (r == null) {                             /* v12d：画布新画件 → 新建入表（启用/未绑定/普通型） */
                r = RegionDraw.make(sh.id, sh.circle, g[0], g[1], g[2], sh.circle ? 0 : g[3]);
                String verr = Regions.validate(r, f, null);
                if (verr != null) throw new IllegalArgumentException(verr);
                f.regions.add(r);
                n++;
                continue;
            }
            if (r.type == 1) { r.a1 = g[0]; r.a2 = g[1]; r.a3 = g[2]; r.a4 = 0; }
            else { r.a1 = g[0]; r.a2 = g[1]; r.a3 = g[2]; r.a4 = g[3]; }
            n++;
        }
        RegionDraw.writeFile(f);
        for (ImageView.RegShape sh : movedSh) sh.moved = false;
        regionsFile = f;
        saveDirty = false;
        boolean running = !Su.corePid().isEmpty();
        return "\u2713 已保存 " + n + " 个区域" + (running ? " · 约 1–2 秒生效" : " · 核心启动时生效");
    }

    /** 有未保存调整时退出确认（继续 / 放弃 / 保存并退出）。 */
    private void askAbandonRegions() {
        final Ui.Dlg d = Ui.dialog(ctx, "有未保存的调整");
        d.body.addView(Ui.dlgMessage(ctx, "全部区域里有尚未保存的调整。"));
        TextView keep = Ui.btnFlat(ctx, "继续调整", Ui.DIM);
        keep.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView drop = Ui.btnFlat(ctx, "放弃", Ui.WARN);
        drop.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                d.dismiss();
                saveDirty = false;
                close();
            }
        });
        TextView saveExit = Ui.btnSmallPrimary(ctx, "保存并退出");
        saveExit.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                d.dismiss();
                Ui.bg(new Runnable() {
                    public void run() {
                        String msg;
                        try { msg = saveAllRegionsSync(); }
                        catch (Throwable t) { msg = "保存失败：" + (t.getMessage() != null ? t.getMessage() : t); }
                        final String fm = msg;
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                Ui.toast(fm);
                                if (fm != null && !fm.startsWith("保存失败")) close();
                            }
                        });
                    }
                }, null);
            }
        });
        d.actions.addView(keep);
        d.actions.addView(drop);
        d.actions.addView(saveExit);
        d.show();
    }

    private MiniNum miniNum(LinearLayout host, String label, int value) {
        MiniNum m = new MiniNum();
        m.row = Ui.row(ctx);
        m.row.setGravity(Gravity.CENTER_VERTICAL);
        m.row.addView(Ui.tv(ctx, label, 12, Ui.DIM, false),
                new LinearLayout.LayoutParams(Ui.dp(ctx, 20), LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView mi = tiny("−");
        mi.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(ctx, 30), Ui.dp(ctx, 34)));
        m.et = new EditText(ctx);
        m.et.setText(String.valueOf(value));
        m.et.setTextSize(13);
        m.et.setTextColor(Ui.TEXT);
        m.et.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED);
        m.et.setBackground(Ui.strokedRound(ctx, Ui.CARD2, Ui.CARD_LINE, 6));
        m.et.setPadding(Ui.dp(ctx, 6), Ui.dp(ctx, 4), Ui.dp(ctx, 6), Ui.dp(ctx, 4));
        LinearLayout.LayoutParams el = new LinearLayout.LayoutParams(Ui.dp(ctx, 56), LinearLayout.LayoutParams.WRAP_CONTENT);
        el.leftMargin = Ui.dp(ctx, 4);
        el.rightMargin = Ui.dp(ctx, 4);
        m.row.addView(m.et, el);
        TextView pl = tiny("＋");
        pl.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(ctx, 30), Ui.dp(ctx, 34)));
        final EditText fe = m.et;
        mi.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpEt(fe, -1); }
        });
        pl.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpEt(fe, 1); }
        });
        m.row.addView(mi);
        m.row.addView(pl);
        m.et.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s2, int st, int c, int a) { }
            public void onTextChanged(CharSequence s2, int st, int b, int c) { }
            public void afterTextChanged(Editable e) { syncBoxFromNums(); }
        });
        host.addView(m.row, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        return m;
    }

    private void bumpEt(EditText et, int d) {
        int v = 0;
        try { v = Integer.parseInt(et.getText().toString().trim()); } catch (Exception ig) { }
        et.setText(String.valueOf(v + d));
    }

    private void bumpThresh(int d) {
        int v = threshVal();
        threshE.setText(String.valueOf(clampI(v + d, 0, 255)));
    }

    private int threshVal() {
        try { return clampI(Integer.parseInt(threshE.getText().toString().trim()), 0, 255); }
        catch (Exception e) { return clampI(s.thresh, 0, 255); }
    }

    // ---------------------------------------------------------------- 数值同步

    private void syncNumsFromBox() {
        if (updatingNums || mX == null) return;
        updatingNums = true;
        int[] b = iv.selection();
        mX.et.setText(String.valueOf(b[0]));
        mY.et.setText(String.valueOf(b[1]));
        mW.et.setText(String.valueOf(b[2]));
        mH.et.setText(String.valueOf(b[3]));
        updatingNums = false;
    }

    private void syncBoxFromNums() {
        if (updatingNums || mX == null || frameRgba == null) return;
        try {
            int x = Integer.parseInt(mX.et.getText().toString().trim());
            int y = Integer.parseInt(mY.et.getText().toString().trim());
            int w = Integer.parseInt(mW.et.getText().toString().trim());
            int h = Integer.parseInt(mH.et.getText().toString().trim());
            updatingNums = true;
            iv.setSelection(x, y, w, h);
            updatingNums = false;
        } catch (Exception ig) { }
    }

    // ---------------------------------------------------------------- 帧加载

    public void attach(final Runnable onReady) {
        Capture.Shot fs = FrameStore.flashShot;                 /* 内存直通：浮标抓完 → 直接用（跳过落盘/解码） */
        if (fs != null && s.frameName == null && System.currentTimeMillis() - FrameStore.flashTs < 30000) {
            FrameStore.flashShot = null;
            useFlash(fs, onReady);
            return;
        }
        if (s.mode == MODE_TMPL_EDIT && s.name != null && editTmpl == null) {
            loadEditTmpl(onReady);
            return;
        }
        if (s.mode == MODE_TMPL_TEST && s.name != null && testTmplGray == null) {
            Ui.bg(new Runnable() {
                public void run() {
                    final Assets.Tmpl t = Assets.readTmpl(s.name);
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            if (t == null) {
                                setResult("读取模板失败：" + s.name, Ui.WARN);
                                return;
                            }
                            testTmplGray = t.gray;
                            testTmplW = t.w;
                            testTmplH = t.h;
                            testTmplRot = t.rot;
                            testTmplAvg = t.avgRgb;
                            testUseFile = true;
                            loadLatest(new Runnable() {
                                public void run() {
                                    if (t.hasXy) iv.setSelection(t.x, t.y, t.w, t.h);
                                    runTest();
                                }
                            });
                        }
                    });
                }
            }, null);
            return;
        }
        if (s.mode == MODE_PTS_TEST && s.name != null && ptsPending == null) {
            Ui.bg(new Runnable() {
                public void run() {
                    final Assets.Pts p = Assets.readPts(s.name);
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            if (p == null) setResult("读取点集失败：" + s.name, Ui.WARN);
                            else ptsPending = p;
                            loadLatest(new Runnable() {
                                public void run() { runPtsTest(); }
                            });
                        }
                    });
                }
            }, null);
            return;
        }
        if (s.mode == MODE_PTS_EDIT && s.name != null && ptsPending == null) {
            Ui.bg(new Runnable() {
                public void run() {
                    final Assets.Pts p = Assets.readPts(s.name);
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            if (p == null) setResult("读取点集失败：" + s.name, Ui.WARN);
                            else ptsPending = p;
                            loadLatest(onReady);
                        }
                    });
                }
            }, null);
            return;
        }
        if (s.mode == MODE_REGIONS && regionsFile == null) {           /* v12c：先读区域表再载帧 */
            Ui.bg(new Runnable() {
                public void run() {
                    Regions.File f = null;
                    String err = null;
                    try { f = RegionDraw.loadLive(); }
                    catch (Throwable t) { err = String.valueOf(t.getMessage() != null ? t.getMessage() : t); }
                    final Regions.File ff = f;
                    final String fe = err;
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            if (fe != null) setResult("读取 regions.conf 失败：" + fe, Ui.WARN);
                            else regionsFile = ff;
                            loadLatest(onReady);
                        }
                    });
                }
            }, null);
            return;
        }
        loadLatest(onReady);
    }

    private void useFlash(final Capture.Shot fs, final Runnable onReady) {
        Ui.bg(new Runnable() {
            public void run() {
                final android.graphics.Bitmap bm = FrameStore.bitmapFromRgba(fs.rgba, fs.w, fs.h);
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        applyFrame(fs.w, fs.h, fs.rot, fs.rgba, bm,
                                "帧 刚刚 · " + fs.w + "×" + fs.h + " · " + (fs.rot == 0 || fs.rot == 2 ? "竖屏" : "横屏"),
                                null, onReady);
                    }
                });
            }
        }, null);
    }

    /** 帧就绪统一入口（文件加载 / 内存直通共用）。 */
    private void applyFrame(int w, int h, int rot, byte[] rgba, Bitmap bm, String label, String name, Runnable onReady) {
        /* v12c：换帧前把总览编辑形状折回逻辑（用户改动跨帧保留） */
        ArrayList<int[]> keepLg = null;
        ArrayList<String> keepId = null;
        ArrayList<boolean[]> keepAtt = null;
        if (s.mode == MODE_REGIONS && iv != null && !iv.regions().isEmpty() && frameRgba != null) {
            keepLg = new ArrayList<int[]>();
            keepId = new ArrayList<String>();
            keepAtt = new ArrayList<boolean[]>();
            for (ImageView.RegShape sh : iv.regions()) {
                int[] lg = sh.circle
                        ? RegionDraw.circleToLogic(frameRot, frameW, frameH, sh.g[0], sh.g[1], sh.g[2])
                        : RegionDraw.rectToLogic(frameRot, frameW, frameH, sh.g[0], sh.g[1], sh.g[2], sh.g[3]);
                keepLg.add(lg);
                keepId.add(sh.id);
                keepAtt.add(new boolean[]{sh.circle, sh.en, sh.moved});
            }
        }
        frameW = w;
        frameH = h;
        frameRot = rot;
        frameRgba = rgba;
        curFrameName = name;
        mfCache = null;
        iv.setFrame(bm, rot);
        int ivMode = ImageView.MODE_POINT;
        if (s.mode == MODE_TMPL_NEW || s.mode == MODE_TMPL_EDIT) ivMode = ImageView.MODE_BOX;
        else if (s.mode == MODE_REGION) ivMode = s.regionCircle ? ImageView.MODE_CIRCLE : ImageView.MODE_BOX;
        else if (s.mode == MODE_REGIONS) ivMode = ImageView.MODE_REGIONS;
        iv.setMode(ivMode);
        iv.setLoupeEnabled(s.mode == MODE_PICK_COLOR);
        frameLine.setText(label);
        if (reBox && editTmpl != null) iv.setSelection(editTmpl.x, editTmpl.y, editTmpl.w, editTmpl.h);
        if ((s.mode == MODE_PTS_NEW || s.mode == MODE_PTS_EDIT || s.mode == MODE_PTS_TEST)
                && ptsPending != null && !ptsPlaced && ptsPending.hasXy) {
            ptsTap(clampI(ptsPending.x, 0, frameW - 1), clampI(ptsPending.y, 0, frameH - 1));
        }
        if ((s.mode == MODE_PICK_POINT || s.mode == MODE_PICK_COLOR) && pendingScreenPick != null) {
            if (System.currentTimeMillis() - pendingScreenPickTs < 60000) {
                applyPick(pendingScreenPick[0], pendingScreenPick[1]);
            }
            pendingScreenPick = null;
        }
        if (s.mode == MODE_REGION && s.regionGeom != null) {   /* v12b：预载表单几何（竖屏逻辑 → 本帧） */
            if (s.regionCircle && s.regionGeom.length >= 3) {
                int[] fc = RegionDraw.circleToFrame(rot, w, h, s.regionGeom[0], s.regionGeom[1], s.regionGeom[2]);
                iv.setCircle(fc[0], fc[1], fc[2]);
            } else if (!s.regionCircle && s.regionGeom.length >= 4) {
                int[] fr2 = Matcher.logicRectToFrame(rot, w, h, s.regionGeom[0], s.regionGeom[1],
                        s.regionGeom[2] - s.regionGeom[0], s.regionGeom[3] - s.regionGeom[1]);
                iv.setSelection(fr2[0], fr2[1], fr2[2], fr2[3]);
            }
            updateRegionResult();
        }
        if (s.mode == MODE_REGIONS) {                          /* v12c：总览投影（优先沿用当前编辑，其次读盘） */
            if (keepLg != null) {
                ArrayList<ImageView.RegShape> shapes = new ArrayList<ImageView.RegShape>();
                for (int i = 0; i < keepLg.size(); i++) {
                    ImageView.RegShape sh = new ImageView.RegShape();
                    sh.id = keepId.get(i);
                    sh.circle = keepAtt.get(i)[0];
                    sh.en = keepAtt.get(i)[1];
                    sh.moved = keepAtt.get(i)[2];
                    projectLogic(sh, keepLg.get(i), rot, w, h);
                    shapes.add(sh);
                }
                iv.setRegions(shapes);
            } else {
                rebuildShapesFromFile();
            }
            if (s.armNew != 0) {          /* v12d：进入即置画具（一次性） */
                iv.setNewArm(s.armNew);
                s.armNew = 0;
            }
            if (s.focusId != null) {      /* v12d：进入即选中所指区域 */
                List<ImageView.RegShape> shs = iv.regions();
                for (int i = 0; i < shs.size(); i++) {
                    if (s.focusId.equals(shs.get(i).id)) { iv.selectRegionForTest(i); break; }
                }
            }
            updateRegionsInfo();
        }
        if (onReady != null) onReady.run();
    }

    private void loadLatest(final Runnable onReady) {
        Ui.bg(new Runnable() {
            public void run() {
                try {
                    loadLatestWorker(onReady);
                } catch (final Throwable t) {
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            setResult("读取帧失败：" + t, Ui.WARN);
                            if (onReady != null) onReady.run();
                        }
                    });
                }
            }
        }, null);
    }

    private void loadLatestWorker(final Runnable onReady) {
        FrameStore.Entry e = null;
                if (s.frameName != null) {
                    for (FrameStore.Entry x : FrameStore.list()) if (x.name.equals(s.frameName)) { e = x; break; }
                }
                if (e == null) e = FrameStore.latest();
                if (e == null) {
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            setResult("帧库还没有帧 —— 点 [去抓帧] 在目标画面抓一张", Ui.WARN);
                            if (onReady != null) onReady.run();
                        }
                    });
                    return;
                }
                final FrameStore.Entry fe = e;
                final Bitmap bm = FrameStore.loadBitmap(e);
                final byte[] rgba = FrameStore.loadRgba(e);
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (bm == null || rgba == null) {
                            setResult("读取帧失败：" + fe.name, Ui.WARN);
                            if (onReady != null) onReady.run();
                            return;
                        }
                        applyFrame(fe.w, fe.h, fe.rot, rgba, bm,
                                "帧 " + shortTime(fe.ts) + " · " + fe.w + "×" + fe.h
                                        + " · " + (fe.rot == 0 || fe.rot == 2 ? "竖屏" : "横屏"),
                                fe.name, onReady);
                    }
                });
    }

    private void loadEditTmpl(final Runnable onReady) {
        Ui.bg(new Runnable() {
            public void run() {
                final Assets.Tmpl t = Assets.readTmpl(s.name);
                if (t == null) {
                    Ui.MAIN.post(new Runnable() {
                        public void run() {
                            setResult("读取模板失败：" + s.name, Ui.WARN);
                            if (onReady != null) onReady.run();
                        }
                    });
                    return;
                }
                final Bitmap prev = previewBitmap(t);
                final byte[] rgba = grayToRgba(t.gray);
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        editTmpl = t;
                        frameW = t.w; frameH = t.h; frameRot = t.rot; frameRgba = rgba;
                        curFrameName = null; mfCache = null;
                        iv.setFrame(prev, t.rot);
                        iv.setMode(ImageView.MODE_BOX);
                        iv.setSelection(0, 0, t.w, t.h);
                        frameLine.setText("模板 " + s.name + " · " + t.w + "×" + t.h
                                + (t.hasXy ? "（原区域 " + t.x + "," + t.y + "）" : ""));
                        if (onReady != null) onReady.run();
                    }
                });
            }
        }, null);
    }

    private static Bitmap previewBitmap(Assets.Tmpl t) {
        int scale = Math.max(1, (Math.max(t.w, t.h) + 1199) / 1200);
        int w = Math.max(1, t.w / scale), h = Math.max(1, t.h / scale);
        int[] px = new int[w * h];
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                int v = t.gray[Math.min(t.h - 1, yy * scale) * t.w + Math.min(t.w - 1, xx * scale)] & 0xFF;
                px[yy * w + xx] = 0xFF000000 | (v << 16) | (v << 8) | v;
            }
        }
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
    }

    private static byte[] grayToRgba(byte[] gray) {
        byte[] out = new byte[gray.length * 4];
        for (int i = 0; i < gray.length; i++) {
            out[i * 4] = gray[i];
            out[i * 4 + 1] = gray[i];
            out[i * 4 + 2] = gray[i];
            out[i * 4 + 3] = (byte) 255;
        }
        return out;
    }

    // ---------------------------------------------------------------- 帧选择 / 抓帧

    private void pickFrameDialog() {
        final List<FrameStore.Entry> ls = FrameStore.list();
        if (ls.isEmpty()) {
            setResult("帧库为空 —— 点 [去抓帧]", Ui.WARN);
            return;
        }
        List<String> items = new ArrayList<String>();
        for (FrameStore.Entry e : ls)
            items.add(shortTime(e.ts) + " · " + e.w + "×" + e.h + " · " + (e.rot == 0 || e.rot == 2 ? "竖" : "横"));
        Forms.pickDialog(ctx, "选择帧", items, 0, new Forms.Pick() {
            public void onPick(int index) {
                s.frameName = ls.get(index).name;
                attach(null);
            }
        });
    }

    private void armCapture() {
        CaptureChip.start(ctx, true);
        if (ctx instanceof Activity) ((Activity) ctx).moveTaskToBack(true);
    }

    // ---------------------------------------------------------------- 试一下 / 保存

    private boolean selectionValid() {
        int[] b = iv.selection();
        return b[2] >= 8 && b[3] >= 8;
    }

    /** 同步计算（可在任意线程）：返回结果行文本，命中细节存 tmpHit/tmpTW/tmpTH。 */
    private String computeTestText() {
        tmpHit = null;
        tmpTW = tmpTH = 0;
        if (frameRgba == null) return "先选帧（[去抓帧] / [用帧]）";
        byte[] gray;
        int tw2, th2, avgRgb2;
        if (testUseFile && testTmplGray != null) {
            gray = testTmplGray;
            tw2 = testTmplW;
            th2 = testTmplH;
            avgRgb2 = testTmplAvg;
            int steps = (testTmplRot - frameRot) & 3;
            if (steps != 0) {
                int[] nwh = new int[2];
                byte[] rg = Matcher.rotateGray(gray, tw2, th2, steps, nwh);
                if (rg == null) return "模板旋转失败";
                gray = rg;
                tw2 = nwh[0];
                th2 = nwh[1];
            }
        } else {
            if (!selectionValid()) return "先在画面上框选模板区域";
            int[] b = iv.selection();
            int[] avg = new int[3];
            gray = Assets.grayFromRgba(frameRgba, frameW, b[0], b[1], b[2], b[3], avg);
            if (gray == null) return "框选区超出帧";
            tw2 = b[2];
            th2 = b[3];
            avgRgb2 = (avg[0] << 16) | (avg[1] << 8) | avg[2];
        }
        if (mfCache == null) mfCache = Matcher.prepare(frameRgba, frameW, frameH, frameRot);
        if (mfCache == null) return "帧视图构建失败";
        int rx, ry, rw, rh;
        if (s.regionRect != null && s.regionRect.length >= 4) {
            int[] fr = Matcher.logicRectToFrame(frameRot, frameW, frameH,
                    s.regionRect[0], s.regionRect[1], s.regionRect[2], s.regionRect[3]);
            rx = fr[0]; ry = fr[1]; rw = fr[2]; rh = fr[3];
        } else {
            rx = 0; ry = 0; rw = frameW; rh = frameH;
        }
        boolean cc = ccRow != null ? ccRow.sw.isChecked() : s.colorCheck;
        long t0 = System.currentTimeMillis();
        Matcher.Hit hit = Matcher.findImage(mfCache, gray, tw2, th2, threshVal(), rx, ry, rw, rh);
        if (hit != null && cc && avgRgb2 >= 0
                && !Matcher.colorCheck(mfCache, hit.x, hit.y, tw2, th2, avgRgb2)) {
            hit = null;                                   /* 颜色校验不过 = 按未命中（同核心口径） */
        }
        long dt = System.currentTimeMillis() - t0;
        if (hit == null) return "未命中（可调阈值重试）";
        tmpHit = hit;
        tmpTW = tw2;
        tmpTH = th2;
        int[] lg = new int[2];
        Matcher.frameToLogic(frameRot, frameW, frameH, hit.x, hit.y, lg);
        return "命中 (" + hit.x + "," + hit.y + ") · 逻辑 (" + lg[0] + "," + lg[1] + ") · " + dt + "ms";
    }

    private void runTest() {
        if (testing || frameRgba == null) {
            if (frameRgba == null) setResult("先选帧（[去抓帧] / [用帧]）", Ui.WARN);
            return;
        }
        testing = true;
        setResult("试查中…", Ui.DIM);
        Ui.bg(new Runnable() {
            public void run() {
                final String txt = computeTestText();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        testing = false;
                        applyTestResult(txt);
                    }
                });
            }
        }, null);
    }

    private void applyTestResult(String txt) {
        if (tmpHit != null) {
            if (tmpTW > 0 && tmpTH > 0) iv.setHit(tmpHit.x, tmpHit.y, tmpTW, tmpTH);
            else iv.setHit(tmpHit.x - 8, tmpHit.y - 8, 16, 16);
        } else iv.setHit(-1, 0, 0, 0);
        setResult(txt, txt.startsWith("命中") ? Ui.OK : Ui.WARN);
    }

    private void setResult(String txt, int color) {
        lastMsg = txt;
        if (resultLine != null) {
            resultLine.setText(txt);
            resultLine.setTextColor(color);
        }
        scrollBarToResult();                           /* v9.6：锁高后结果可能滚出可视区——更新即带回 */
    }

    private void onPrimary() {
        if (s.mode == MODE_VIEW) { close(); return; }
        if (s.mode == MODE_REGION) { regionConfirm(); return; }   /* v12b */
        if (s.mode == MODE_PICK_POINT || s.mode == MODE_PICK_COLOR) { pickConfirm(); return; }
        if (s.mode == MODE_PTS_NEW || s.mode == MODE_PTS_EDIT) {
            final String name = nameE.getText().toString().trim();
            boolean sameEdit = s.mode == MODE_PTS_EDIT && name.equals(s.name);
            if (!sameEdit && Assets.exists(name)) {
                askOverwrite(name, new Runnable() {
                    public void run() {
                        if (savePts(name)) runPtsTest();
                    }
                });
                return;
            }
            if (savePts(name)) runPtsTest();
            return;
        }
        if (!save(true)) return;
        runTest();
    }

    /** 覆盖确认（模板/点集共用入口）。 */
    private void askOverwrite(final String name, final Runnable onYes) {
        final Ui.Dlg d = Ui.dialog(ctx, "覆盖素材？");
        d.body.addView(Ui.dlgMessage(ctx, "已存在同名素材「" + name + "」（模板/点集共用名字空间）。覆盖它？"));
        TextView no = Ui.btnFlat(ctx, "取消", Ui.DIM);
        no.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView yes = Ui.btnSmallPrimary(ctx, "覆盖");
        yes.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                d.dismiss();
                onYes.run();
            }
        });
        d.actions.addView(no);
        d.actions.addView(yes);
        d.show();
    }

    /** 保存模板；confirmOverwrite=true 时对已存在名字弹确认（编辑同名不弹）。 */
    private boolean save(boolean confirmOverwrite) {
        if (frameRgba == null) { setResult("先选帧（[去抓帧] / [用帧]）", Ui.WARN); return false; }
        if (!selectionValid()) { setResult("先在画面上框选模板区域", Ui.WARN); return false; }
        final String name = nameE.getText().toString().trim();
        if (!Assets.nameOk(name)) { setResult("名称不合法（1..15 个可见字符，可用中文）", Ui.WARN); return false; }
        boolean sameEdit = s.mode == MODE_TMPL_EDIT && name.equals(s.name);
        if (!sameEdit && Assets.exists(name) && confirmOverwrite) {
            final Ui.Dlg d = Ui.dialog(ctx, "覆盖素材？");
            d.body.addView(Ui.dlgMessage(ctx, "已存在同名素材「" + name + "」（模板/点集共用名字空间）。覆盖它？"));
            TextView no = Ui.btnFlat(ctx, "取消", Ui.DIM);
            no.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { d.dismiss(); }
            });
            TextView yes = Ui.btnSmallPrimary(ctx, "覆盖");
            yes.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    d.dismiss();
                    if (save(false)) runTest();
                }
            });
            d.actions.addView(no);
            d.actions.addView(yes);
            d.show();
            return false;
        }
        int[] b = iv.selection();
        int[] avg = new int[3];
        byte[] gray = Assets.grayFromRgba(frameRgba, frameW, b[0], b[1], b[2], b[3], avg);
        if (gray == null) { setResult("框选区超出帧", Ui.WARN); return false; }
        Assets.Tmpl t = new Assets.Tmpl();
        t.w = b[2];
        t.h = b[3];
        t.rot = frameRot;
        t.x = b[0];
        t.y = b[1];
        t.hasXy = true;
        t.avgRgb = (avg[0] << 16) | (avg[1] << 8) | avg[2];
        t.gray = gray;
        Assets.writeTmpl(name, t);
        s.mode = MODE_TMPL_EDIT;
        s.name = name;
        editTmpl = t;
        titleT.setText("编辑模板 " + name);
        if (done != null) done.onTmplSaved(name);
        setResult("已保存 " + name, Ui.OK);
        Ui.toast("已保存 " + name);
        return true;
    }

    /** 保存 + 试一下（无覆盖确认；测试/一键路径）；返回结果行文本。 */
    String saveAndTest() {
        if (!save(false)) return "保存失败：" + lastMsg;
        String txt = computeTestText();
        applyTestResult(txt);
        return txt;
    }

    // ---------------------------------------------------------------- 取点/吸色入口（T7 补齐交互）

    void onIvPoint(int x, int y) {
        if (s.mode == MODE_PICK_POINT) {
            pickedFx = x;
            pickedFy = y;
            int[] lg = new int[2];
            Matcher.frameToLogic(frameRot, frameW, frameH, x, y, lg);
            setResult("帧 (" + x + "," + y + ") · 逻辑 (" + lg[0] + "," + lg[1] + ") · [确认]", Ui.TEXT);
            enableConfirm();
        } else if (s.mode == MODE_PICK_COLOR) {
            pickedFx = x;
            pickedFy = y;
            lastPickRgb = sampleRgb(x, y);
            setResult(String.format(java.util.Locale.US, "颜色 #%06X @(%d,%d) · [确认]", lastPickRgb, x, y), Ui.TEXT);
            if (swatch != null) swatch.setBackground(Ui.strokedRound(ctx, 0xFF000000 | lastPickRgb, Ui.CARD_LINE, 4));
            enableConfirm();
        } else if (s.mode == MODE_PTS_NEW || s.mode == MODE_PTS_EDIT || s.mode == MODE_PTS_TEST) {
            ptsTap(x, y);
        }
    }

    private String suggestName() {
        if (s.mode == MODE_TMPL_EDIT || s.mode == MODE_PTS_EDIT) return s.name == null ? "" : s.name;
        String prefix = (s.mode == MODE_PTS_NEW || s.mode == MODE_PTS_EDIT) ? "pts" : "tmpl";
        List<String> all = new ArrayList<String>();
        all.addAll(Assets.namesTmpl());
        all.addAll(Assets.namesPts());
        for (int i = 1; i < 1000; i++) {
            String c = prefix + i;
            if (!all.contains(c)) return c;
        }
        return prefix;
    }

    private static String shortTime(long ts) {
        return new java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.US).format(new java.util.Date(ts));
    }

    private static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // ---------------- T7：取点/吸色确认 + 点集（基准/参考点/保存/试一下/模糊提示） ----------------

    int lastPickRgb = -1;
    private boolean blurWarn;

    void pickConfirm() {
        if (s.mode == MODE_PICK_POINT && pickedFx >= 0) {
            int[] lg = new int[2];
            Matcher.frameToLogic(frameRot, frameW, frameH, pickedFx, pickedFy, lg);
            if (done != null) done.onPointPicked(lg[0], lg[1]);
            closeInternal(false);
        } else if (s.mode == MODE_PICK_COLOR && lastPickRgb >= 0) {
            if (done != null) done.onColorPicked(lastPickRgb);
            closeInternal(false);
        }
    }

    private void enableConfirm() {
        if (confirmB != null) {
            confirmB.setEnabled(true);
            confirmB.setAlpha(1f);
        }
    }

    void ptsTestTap(int x, int y) { ptsTap(x, y); }

    int ptsCount() { return ptsList.size(); }

    int[] frameInfoForTest() { return new int[]{frameW, frameH, frameRot}; }

    boolean lastBlurWarn() { return blurWarn; }

    String resultText() { return resultLine == null ? "" : resultLine.getText().toString(); }

    int[] pickedForTest() { return new int[]{pickedFx, pickedFy}; }

    private int sampleRgb(int x, int y) {
        if (frameRgba == null || x < 0 || y < 0 || x >= frameW || y >= frameH) return 0;
        int p = (y * frameW + x) * 4;
        return ((frameRgba[p] & 0xFF) << 16) | ((frameRgba[p + 1] & 0xFF) << 8) | (frameRgba[p + 2] & 0xFF);
    }

    private void ptsTap(int x, int y) {
        if (frameRgba == null) return;
        if (ptsBaseX < 0) {
            ptsBaseX = x;
            ptsBaseY = y;
            ptsBaseRgb = sampleRgb(x, y);
            iv.setBase(x, y, ptsBaseRgb);
            if (ptsPending != null && !ptsPlaced) {
                for (Assets.Pt pt : ptsPending.pts) {
                    ptsList.add(new int[]{clampI(x + pt.dx, 0, frameW - 1), clampI(y + pt.dy, 0, frameH - 1),
                            pt.rgb, pt.tol});
                }
                ptsPlaced = true;
            } else if (reBaseX >= 0) {
                int dx = x - reBaseX, dy = y - reBaseY;
                for (int[] q : ptsList) {
                    q[0] = clampI(q[0] + dx, 0, frameW - 1);
                    q[1] = clampI(q[1] + dy, 0, frameH - 1);
                }
            }
            reBaseX = -1;
            refreshBaseLine();
            refreshPtsRows();
            refreshMarkers();
            setResult(String.format(java.util.Locale.US, "基准 #%06X @(%d,%d) 已取", ptsBaseRgb, ptsBaseX, ptsBaseY), Ui.TEXT);
            scrollBarToTop();
        } else {
            if (ptsList.size() >= 16) {
                setResult("参考点已满（16 个上限）", Ui.WARN);
                return;
            }
            ptsList.add(new int[]{x, y, sampleRgb(x, y), ptsBaseTol});
            refreshPtsRows();
            refreshMarkers();
            setResult("参考点 " + ptsList.size() + "（Δ" + (x - ptsBaseX) + "," + (y - ptsBaseY) + "）已加", Ui.TEXT);
        }
    }

    private void refreshMarkers() {
        int[] xs = new int[ptsList.size()];
        int[] ys = new int[ptsList.size()];
        for (int i = 0; i < ptsList.size(); i++) {
            xs[i] = ptsList.get(i)[0];
            ys[i] = ptsList.get(i)[1];
        }
        iv.setMarkers(xs, ys);
    }

    private void refreshBaseLine() {
        if (ptsBaseLine == null) return;
        if (ptsBaseX < 0) ptsBaseLine.setText("基准：点一下画面取基准色（自动吸色）");
        else ptsBaseLine.setText(String.format(java.util.Locale.US,
                "基准 #%06X @(%d,%d) · 基差 %d", ptsBaseRgb, ptsBaseX, ptsBaseY, ptsBaseTol));
    }

    private void refreshPtsRows() {
        if (ptsRows == null) return;
        ptsRows.removeAllViews();
        for (int i = 0; i < ptsList.size(); i++) {
            final int idx = i;
            final int[] q = ptsList.get(i);
            LinearLayout r = Ui.row(ctx);
            r.setGravity(Gravity.CENTER_VERTICAL);
            TextView t = Ui.mono(ctx, (i + 1) + " Δ" + (q[0] - ptsBaseX) + "," + (q[1] - ptsBaseY)
                    + " #" + String.format(java.util.Locale.US, "%06X", q[2]) + " t" + q[3], 11.5f, Ui.DIM);
            r.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView mi = tiny("−");
            TextView pl = tiny("＋");
            TextView del = tiny("✕");
            mi.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    q[3] = clampI(q[3] - 1, 0, 255);
                    refreshPtsRows();
                }
            });
            pl.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    q[3] = clampI(q[3] + 1, 0, 255);
                    refreshPtsRows();
                }
            });
            del.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    ptsList.remove(idx);
                    refreshPtsRows();
                    refreshMarkers();
                }
            });
            r.addView(mi);
            r.addView(pl);
            r.addView(del);
            ptsRows.addView(r);
        }
        if (ptsList.isEmpty()) {
            ptsRows.addView(Ui.tv(ctx, "（还没加参考点——在画面上点参考位置，逐点容差可调）", 11, Ui.FAINT, false));
        } else {
            scrollBarToRowsEnd();                       /* v9.6：锁高栏——新增/删除后列表尾自动可见 */
        }
    }

    /** 试一下（模板）专用底栏：阈值快捷行 + [试一下] + 结果行。 */
    private LinearLayout buildTmplTestBar() {
        LinearLayout bar = Ui.col(ctx);
        bar.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 6), Ui.dp(ctx, 12), Ui.dp(ctx, 8));
        LinearLayout tr = Ui.row(ctx);
        tr.setGravity(Gravity.CENTER_VERTICAL);
        tr.addView(Ui.tv(ctx, "试查阈值", 12, Ui.DIM, false));
        TextView tm10 = tiny("−10");
        TextView tm1 = tiny("−1");
        threshE = new EditText(ctx);
        threshE.setText(String.valueOf(clampI(s.thresh, 0, 255)));
        threshE.setTextSize(13);
        threshE.setTextColor(Ui.TEXT);
        threshE.setInputType(InputType.TYPE_CLASS_NUMBER);
        threshE.setBackground(Ui.strokedRound(ctx, Ui.CARD2, Ui.CARD_LINE, 6));
        threshE.setPadding(Ui.dp(ctx, 6), Ui.dp(ctx, 4), Ui.dp(ctx, 6), Ui.dp(ctx, 4));
        LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(Ui.dp(ctx, 48), LinearLayout.LayoutParams.WRAP_CONTENT);
        tl.leftMargin = Ui.dp(ctx, 6);
        tl.rightMargin = Ui.dp(ctx, 6);
        tr.addView(threshE, tl);
        TextView tp1 = tiny("＋1");
        TextView tp10 = tiny("＋10");
        tm10.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(-10); } });
        tm1.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(-1); } });
        tp1.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(1); } });
        tp10.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { bumpThresh(10); } });
        tr.addView(tm10);
        tr.addView(tm1);
        tr.addView(tp1);
        tr.addView(tp10);
        bar.addView(tr);
        resultLine = Ui.mono(ctx, "", 12, Ui.DIM);
        bar.addView(resultLine);
        LinearLayout ba = Ui.row(ctx);
        TextView test = Ui.btnSmallPrimary(ctx, "试一下");
        test.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runTest(); }
        });
        ba.addView(test);
        bar.addView(ba);
        return bar;
    }

    private LinearLayout buildPtsBar() {
        LinearLayout bar = Ui.col(ctx);
        bar.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 4), Ui.dp(ctx, 12), Ui.dp(ctx, 8));
        final boolean testOnly = s.mode == MODE_PTS_TEST;
        LinearLayout nr = Ui.row(ctx);
        if (testOnly) {
            bar.addView(Ui.tv(ctx, "试一下（点画面可重取基准；改完点 [试一下]）", 11, Ui.FAINT, false));
        } else {
        nr.addView(Ui.tv(ctx, "名称", 13, Ui.TEXT, false));
        nameE = Ui.textField(ctx, "如 pts1");
        nameE.setText(suggestName());
        LinearLayout.LayoutParams nl = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        nl.leftMargin = Ui.dp(ctx, 8);
        nr.addView(nameE, nl);
        if (s.mode == MODE_PTS_EDIT) {
            TextView re = Ui.btnSmall(ctx, "重选基准");
            re.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if (ptsBaseX >= 0) {
                        reBaseX = ptsBaseX;
                        reBaseY = ptsBaseY;
                        ptsBaseX = -1;
                        refreshBaseLine();
                        setResult("点一下画面选新基准（参考点整组跟随）", Ui.TEXT);
                    }
                }
            });
            nr.addView(re);
        }
        bar.addView(nr);
        }
        LinearLayout bl = Ui.row(ctx);
        bl.setGravity(Gravity.CENTER_VERTICAL);
        ptsBaseLine = Ui.mono(ctx, "", 11.5f, Ui.DIM);
        bl.addView(ptsBaseLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        bl.addView(Ui.tv(ctx, "基差", 11, Ui.DIM, false));
        TextView bm = tiny("−");
        TextView bp = tiny("＋");
        bm.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                ptsBaseTol = clampI(ptsBaseTol - 1, 0, 255);
                refreshBaseLine();
            }
        });
        bp.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                ptsBaseTol = clampI(ptsBaseTol + 1, 0, 255);
                refreshBaseLine();
            }
        });
        bl.addView(bm);
        bl.addView(bp);
        bar.addView(bl);
        refreshBaseLine();
        ptsRows = Ui.col(ctx);
        bar.addView(ptsRows);
        resultLine = Ui.mono(ctx, "", 12, Ui.DIM);
        bar.addView(resultLine);
        LinearLayout ba = Ui.row(ctx);
        TextView test = Ui.btnSmall(ctx, "试一下");
        test.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runPtsTest(); }
        });
        ba.addView(test);
        if (!testOnly) {
            TextView st = Ui.btnSmallPrimary(ctx, "保存并试一下");
            st.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { onPrimary(); }
            });
            ba.addView(st);
        }
        bar.addView(ba);
        return bar;
    }

    private boolean savePts(String name) {
        if (ptsBaseX < 0) {
            setResult("先点一下画面取基准色", Ui.WARN);
            return false;
        }
        if (ptsList.isEmpty()) {
            setResult("先在画面上加至少一个参考点", Ui.WARN);
            return false;
        }
        if (!Assets.nameOk(name)) {
            setResult("名称不合法（1..15 个可见字符，可用中文）", Ui.WARN);
            return false;
        }
        Assets.Pts p = new Assets.Pts();
        p.baseRgb = ptsBaseRgb;
        p.baseTol = ptsBaseTol;
        p.x = ptsBaseX;
        p.y = ptsBaseY;
        p.hasXy = true;
        p.pts = new Assets.Pt[ptsList.size()];
        for (int i = 0; i < ptsList.size(); i++) {
            int[] q = ptsList.get(i);
            Assets.Pt pt = new Assets.Pt();
            pt.dx = q[0] - ptsBaseX;
            pt.dy = q[1] - ptsBaseY;
            pt.rgb = q[2];
            pt.tol = q[3];
            p.pts[i] = pt;
        }
        Assets.writePts(name, p);
        s.mode = MODE_PTS_EDIT;
        s.name = name;
        titleT.setText("编辑点集 " + name);
        if (done != null) done.onTmplSaved(name);
        setResult("已保存 " + name, Ui.OK);
        Ui.toast("已保存 " + name);
        return true;
    }

    private String computePtsTestText() {
        tmpHit = null;
        tmpTW = tmpTH = 0;
        if (frameRgba == null) return "先选帧（[去抓帧] / [用帧]）";
        if (ptsBaseX < 0 || ptsList.isEmpty()) return "先取基准色并加参考点";
        if (mfCache == null) mfCache = Matcher.prepare(frameRgba, frameW, frameH, frameRot);
        blurWarn = false;
        int cnt = 0;
        int tr = (ptsBaseRgb >> 16) & 0xFF, tg = (ptsBaseRgb >> 8) & 0xFF, tb = ptsBaseRgb & 0xFF;
        outer:
        for (int y = 0; y < frameH; y += 2) {
            int rowB = y * frameW * 4;
            for (int x = 0; x < frameW; x += 2) {
                int p = rowB + x * 4;
                int dr = (frameRgba[p] & 0xFF) - tr;
                if (dr < 0) dr = -dr;
                int dg = (frameRgba[p + 1] & 0xFF) - tg;
                if (dg < 0) dg = -dg;
                int db = (frameRgba[p + 2] & 0xFF) - tb;
                if (db < 0) db = -db;
                if (dr <= ptsBaseTol && dg <= ptsBaseTol && db <= ptsBaseTol) {
                    cnt++;
                    if (cnt > 500) break outer;
                }
            }
        }
        blurWarn = cnt > 200;
        String warn = blurWarn ? " · 该色出现过多，建议加点集约束" : "";
        Matcher.Pt[] mpts = new Matcher.Pt[ptsList.size()];
        for (int i = 0; i < ptsList.size(); i++) {
            int[] q = ptsList.get(i);
            Matcher.Pt pt = new Matcher.Pt();
            pt.dx = q[0] - ptsBaseX;
            pt.dy = q[1] - ptsBaseY;
            pt.rgb = q[2];
            pt.tol = q[3];
            mpts[i] = pt;
        }
        int rx, ry, rw, rh;
        if (s.regionRect != null && s.regionRect.length >= 4) {
            int[] fr = Matcher.logicRectToFrame(frameRot, frameW, frameH,
                    s.regionRect[0], s.regionRect[1], s.regionRect[2], s.regionRect[3]);
            rx = fr[0];
            ry = fr[1];
            rw = fr[2];
            rh = fr[3];
        } else {
            rx = 0;
            ry = 0;
            rw = frameW;
            rh = frameH;
        }
        long t0 = System.currentTimeMillis();
        Matcher.Hit hit = Matcher.findColorMulti(mfCache, rx, ry, rw, rh, ptsBaseRgb, ptsBaseTol, mpts, mpts.length);
        long dt = System.currentTimeMillis() - t0;
        if (hit == null) return "未命中（该位置没找到基准色+参考点组合） · " + dt + "ms" + warn;
        tmpHit = hit;
        int[] lg = new int[2];
        Matcher.frameToLogic(frameRot, frameW, frameH, hit.x, hit.y, lg);
        return "命中 (" + hit.x + "," + hit.y + ") · 逻辑 (" + lg[0] + "," + lg[1] + ") · " + dt + "ms" + warn;
    }

    private void runPtsTest() {
        if (testing || frameRgba == null) {
            if (frameRgba == null) setResult("先选帧（[去抓帧] / [用帧]）", Ui.WARN);
            return;
        }
        testing = true;
        setResult("试查中…", Ui.DIM);
        Ui.bg(new Runnable() {
            public void run() {
                final String txt = computePtsTestText();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        testing = false;
                        applyTestResult(txt);
                    }
                });
            }
        }, null);
    }

    String ptsSaveAndTest(String name) {
        if (!savePts(name)) return "保存失败：" + lastMsg;
        String txt = computePtsTestText();
        applyTestResult(txt);
        return txt;
    }
}
