package com.vtouch.editor;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * 编辑悬浮窗（v8，浮标 + 面板）：
 *  · 武装态（编辑器 [去抓帧]）：单击=抓帧（自隐→raw→闪存直通→立即回编辑器，落盘后台继续）；长按=开面板。
 *  · 面板态（素材页 [悬浮试一下] 或长按）：单击=开/合面板。面板 = 编辑工具箱：
 *      [取图]=抓帧→直接新建模板 · [抓帧]=抓帧→帧库并回编辑器 · [取色]=全屏遮罩吞一次触摸 → 实时色值卡
 *      · 测试找图/找色 = 选模板/点集 → 在当前屏幕实时找一次（不存帧，命中框标屏上）
 *      · 脚本测试 = 选操作 → 干跑（评估各视觉步命中，不执行触摸）→ 逐行报告 + 命中框标屏上。
 *  · 抓帧机制：自隐（窗口 alpha=0，保持 SAW 可见窗）→ su screencap raw → 闪存直通；回跳前恢复可见再拉起。
 */
public class CaptureChip extends Service {
    private final Handler h = new Handler(Looper.getMainLooper());
    private WindowManager wm;
    private WindowManager.LayoutParams lp;
    private FrameLayout chip;
    private android.widget.ImageView chipMark;
    private TextView chipGlyph;
    private boolean added = false;
    private boolean armed = false;
    private boolean capturing = false;
    private boolean chipLeft = false;          /* 贴左边（默认右） */
    private boolean collapsed = false;         /* 靠边收起态 */
    private View chipDot;                      /* 收起态的小圆点 */

    private LinearLayout testPanel;
    private TextView panelResult;
    private ScrollView panelScroll;
    private boolean panelAdded = false;
    private List<String> pTmpls = new ArrayList<String>();
    private List<String> pPtss = new ArrayList<String>();
    private List<String> pOps = new ArrayList<String>();
    private String pickKind = "point";
    private int panelView = 0;                 /* 0=主 1=测试素材 2=脚本干跑 3=报告 */
    private int quickThresh = 8;
    private final Map<String, String[]> qres = new HashMap<String, String[]>();   /* name|0/1 → {胶囊文案, 色键, 明细} */
    private final Map<String, TextView[]> qrowViews = new HashMap<String, TextView[]>();               /* 测素材的试查阈值（仅影响悬浮窗实时测试） */
    private WindowManager.LayoutParams panelLp;   /* 面板窗口参数（随浮标拖动重定位） */
    private LinearLayout closingPanel;             /* 收起动画中的面板（清内容→帧提交→卸窗链路的持有者；非空 = 正在关闭） */
    private float panelDownX, panelDownY;         /* 拖动开始时面板位置（跟随位移基准） */
    private static final List<String> recentAssets = new ArrayList<String>();   /* "name|0|1"，最多 3 */

    private View pickOverlay;
    private boolean pickAdded = false;
    private boolean pickMode = false;
    private float pickDownX, pickDownY;
    private boolean pickMoved;

    private float downRawX, downRawY;
    private int downLpX, downLpY;
    private boolean moved;
    private boolean longFired;

    private final Runnable longPress = new Runnable() {
        public void run() {
            longFired = true;
            if (collapsed) hideAll("collapsed-exit");
            else openTestPanel();
        }
    };

    private final Runnable paintBack = new Runnable() {
        public void run() { paintChip(); }
    };

    /** 从编辑器 [去抓帧] 拉起；arm=true 武装态（单击即抓帧并回编辑器）。 */
    public static void start(Context c, boolean arm) {
        Intent i = new Intent(c, CaptureChip.class);
        i.putExtra("arm", arm);
        try {
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Throwable t) { }
    }

    /** 素材页 [悬浮试一下]：拉起并直接展开测试面板（点素材 = 在当前屏幕实时找）。 */
    public static void startPanel(Context c) {
        Intent i = new Intent(c, CaptureChip.class);
        i.putExtra("panel", true);
        try {
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Throwable t) { }
    }

    /** 是否在屏（侧边栏开关状态用）。 */
    public static volatile boolean running = false;

    public static boolean isRunning() { return running; }

    /** 侧边栏开关-关：停服务（窗口随 onDestroy 一并撤走）。 */
    public static void stop(Context c) {
        try {
            c.stopService(new Intent(c, CaptureChip.class));
        } catch (Throwable t) { }
    }

    /** 取点会话 [屏幕取点]：全屏遮罩吞一次触摸 → 抓帧 → 回编辑器回填。 */
    public static void startPick(Context c) { startPick(c, "point"); }

    /** kind = point|color（无会话在场时决定结果会话形态）。 */
    public static void startPick(Context c, String kind) {
        Intent i = new Intent(c, CaptureChip.class);
        i.putExtra("pick", true);
        if (kind != null) i.putExtra("pickKind", kind);
        try {
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Throwable t) { }
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        ProbeService.startAsForeground(this, "vtouch 浮标");
        Su.init(getCacheDir().getAbsolutePath());
        Capture.init(this);
        FrameStore.init(this);
        if (intent != null && intent.getBooleanExtra("arm", false)) armed = true;
        boolean wantPick = intent != null && intent.getBooleanExtra("pick", false);
        boolean wantPanel = intent != null && intent.getBooleanExtra("panel", false);
        if (intent != null && intent.getStringExtra("pickKind") != null) pickKind = intent.getStringExtra("pickKind");
        TestBench.appendLog(this, "chip_debug.txt", "onStart arm=" + armed + " pick=" + wantPick
                + " panel=" + wantPanel + " added=" + added + " pickAdded=" + pickAdded);
        if (wantPick) {
            pickMode = true;
            if (!added) addChip();
            setChipAlpha(0f);
            addPickOverlay();
        } else {
            if (!added) addChip();
            if (wantPanel && !panelAdded) openTestPanel();
        }
        if (intent != null && intent.getBooleanExtra("capture", false)) doCapture();
        final String demo = intent != null ? intent.getStringExtra("demo") : null;   /* 调试：程序化开/关（零注入录屏复现用） */
        if ("open".equals(demo)) {
            if (!panelAdded) openTestPanel();
        } else if ("close".equals(demo)) {
            if (panelAdded) closeTestPanelAnimated();
        } else if ("cycle".equals(demo)) {
            if (!panelAdded) openTestPanel();
            h.postDelayed(new Runnable() {
                public void run() { if (panelAdded) closeTestPanelAnimated(); }
            }, 1200);
        } else if ("assets".equals(demo) || "dry".equals(demo)) {
            if (!panelAdded) openTestPanel();
            final int gv = "assets".equals(demo) ? 1 : 2;
            for (int k = 1; k <= 6; k++) {                   /* 素材加载异步：多次重试直到面板就绪 */
                h.postDelayed(new Runnable() {
                    public void run() { if (panelAdded && panelScroll != null) gotoView(gv); }
                }, 400L * k);
            }
        }
        return START_NOT_STICKY;
    }

    private void addChip() {
        try {
            wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            chip = new FrameLayout(this);
            chip.setMinimumWidth(Ui.dp(this, 52));
            chip.setMinimumHeight(Ui.dp(this, 52));
            chip.setClickable(true);
            chipMark = new android.widget.ImageView(this);
            FrameLayout.LayoutParams ml = new FrameLayout.LayoutParams(Ui.dp(this, 26), Ui.dp(this, 26));
            ml.gravity = Gravity.CENTER;
            chip.addView(chipMark, ml);
            chipGlyph = new TextView(this);
            chipGlyph.setTextSize(17);
            chipGlyph.setGravity(Gravity.CENTER);
            chipGlyph.setVisibility(View.GONE);
            chip.addView(chipGlyph, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
            chipDot = new View(this);
            chipDot.setVisibility(View.GONE);
            chipDot.setBackground(Ui.round(this, Ui.ACCENT, 5));
            chip.addView(chipDot, new FrameLayout.LayoutParams(Ui.dp(this, 10), Ui.dp(this, 10), Gravity.CENTER));
            paintChip();
            chip.setOnTouchListener(new View.OnTouchListener() {
                public boolean onTouch(View v, MotionEvent e) {
                    switch (e.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:
                            downRawX = e.getRawX();
                            downRawY = e.getRawY();
                            downLpX = lp.x;
                            downLpY = lp.y;
                            panelDownX = panelLp != null ? panelLp.x : 0;
                            panelDownY = panelLp != null ? panelLp.y : 0;
                            moved = false;
                            longFired = false;
                            h.postDelayed(longPress, 550);
                            return true;
                        case MotionEvent.ACTION_MOVE: {
                            float dx = e.getRawX() - downRawX;
                            float dy = e.getRawY() - downRawY;
                            if (Math.abs(dx) + Math.abs(dy) > Ui.dp(CaptureChip.this, 12)) {
                                moved = true;
                                h.removeCallbacks(longPress);
                                lp.x = downLpX - Math.round(dx);
                                lp.y = downLpY + Math.round(dy);
                                try { wm.updateViewLayout(chip, lp); } catch (Throwable ig) { }
                                if (panelAdded && panelLp != null && testPanel != null) {
                                    panelLp.x = (int) (panelDownX + Math.round(dx));   /* 面板与浮标成对移动 */
                                    panelLp.y = (int) (panelDownY + Math.round(dy));
                                    clampPanel(panelLp);
                                    try { wm.updateViewLayout(testPanel, panelLp); } catch (Throwable ig) { }
                                }
                            }
                            return true;
                        }
                        case MotionEvent.ACTION_UP:
                            h.removeCallbacks(longPress);
                            if (!moved && !longFired) {
                                if (collapsed) {
                                    setCollapsed(false);
                                    if (!armed) openTestPanel();
                                } else if (armed) {
                                    doCapture();
                                } else {
                                    openTestPanel();
                                }
                            } else if (moved) {
                                if (!panelAdded) snapToEdge();   /* 面板开着时：成对移动，不收起/不吸附 */
                                saveChipPos();
                            }
                            longFired = false;
                            return true;
                        case MotionEvent.ACTION_CANCEL:
                            h.removeCallbacks(longPress);
                            if (moved) saveChipPos();
                            longFired = false;
                            return true;
                    }
                    return false;
                }
            });
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            lp = new WindowManager.LayoutParams(
                    Ui.dp(this, 52),
                    Ui.dp(this, 52),
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                    PixelFormat.TRANSLUCENT);
            Point rsz0 = new Point();
            wm.getDefaultDisplay().getRealSize(rsz0);
            android.content.SharedPreferences sp = getSharedPreferences("chip", MODE_PRIVATE);
            chipLeft = "left".equals(sp.getString("side", ""));
            lp.gravity = (chipLeft ? Gravity.LEFT : Gravity.RIGHT) | Gravity.TOP;
            lp.x = Ui.dp(this, 12);
            lp.y = Math.max(0, Math.min(sp.getInt("y", (int) (rsz0.y * 0.30f)), rsz0.y - Ui.dp(this, 64)));
            wm.addView(chip, lp);
            added = true;
            running = true;
            TestBench.appendLog(this, "chip_debug.txt", "addChip ok");
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "浮标添加失败（权限缺失？）: " + t);
        }
    }

    private void paintChip() {
        if (chip == null) return;
        if (chipGlyph != null) chipGlyph.setVisibility(View.GONE);
        if (chipMark != null) {
            chipMark.setVisibility(View.VISIBLE);
            chipMark.setImageDrawable(Ui.brandMark(this, armed ? Ui.ON_ACCENT : Ui.ACCENT));
        }
        if (armed) {
            chip.setBackground(Ui.pressable(this, Ui.ACCENT, 0x40FFFFFF, 16));          /* 琥珀实底：待抓 */
        } else {
            chip.setBackground(Ui.pressableStroked(this, 0xF21D2126, Ui.CARD_LINE, 0x40FFFFFF, 16));
        }
    }

    /** 抓帧（默认：回编辑器看新帧）。 */
    private void doCapture() { doCapture(0); }

    /** 抓帧（任何入口）：自隐 → 抓 → 闪存直通 → 立即回编辑器；落盘后台继续。
     *  into=0 看新帧（查看器）；into=1 直接开「新建模板」会话（取图流程）。 */
    private void doCapture(final int into) {
        if (capturing || chip == null || closingPanel != null) return;
        capturing = true;
        TestBench.appendLog(this, "chip_debug.txt", "doCapture start armed=" + armed + " into=" + into);
        setChipAlpha(0f);
        if (panelAdded && testPanel != null) testPanel.setVisibility(View.GONE);
        h.postDelayed(new Runnable() {
            public void run() {
                new Thread(new Runnable() {
                    public void run() {
                        Capture.Shot shot = null;
                        String msg = null;
                        try {
                            shot = Capture.grab();
                        } catch (Exception ex) {
                            msg = ex.getMessage();
                        }
                        final Capture.Shot fshot = shot;
                        final String fm = msg;
                        if (fshot != null) {
                            FrameStore.flashShot = fshot;
                            FrameStore.flashTs = System.currentTimeMillis();
                        }
                        h.post(new Runnable() {
                            public void run() {
                                capturing = false;
                                if (fshot == null) {
                                    if (chip != null && added) {
                                        syncChipVisibility();
                                        setText("\u2715", Ui.WARN);
                                        h.postDelayed(paintBack, 1500);
                                    }
                                    if (panelAdded && testPanel != null) testPanel.setVisibility(View.VISIBLE);
                                    TestBench.appendLog(CaptureChip.this, "chip_error.txt", "抓帧失败: " + fm);
                                    return;
                                }
                                TestBench.appendLog(CaptureChip.this, "chip_debug.txt",
                                        "doCapture ok " + fshot.w + "x" + fshot.h);
                                setText("\u2713", Ui.OK);
                                launchEditor(-1, -1, into);
                            }
                        });
                        if (fshot != null) {
                            try {
                                FrameStore.save(fshot);
                            } catch (Throwable t) {
                                TestBench.appendLog(CaptureChip.this, "chip_error.txt", "落盘失败: " + t);
                            }
                        }
                    }
                }, "chip-capture").start();
            }
        }, 120);
    }

    /** 记住浮标位置（边 + y；跨会话）。 */
    private void saveChipPos() {
        if (lp == null) return;
        try {
            getSharedPreferences("chip", MODE_PRIVATE).edit()
                    .putString("side", chipLeft ? "left" : "right")
                    .putInt("y", lp.y).apply();
        } catch (Throwable ig) { }
    }

    /** 拖动结束：吸附到最近的一条竖边，并靠边收起。 */
    private void snapToEdge() {
        try {
            Point rsz = new Point();
            wm.getDefaultDisplay().getRealSize(rsz);
            int wNow = chip != null && chip.getWidth() > 0 ? chip.getWidth() : Ui.dp(this, 52);
            int left;
            if (chipLeft) left = lp.x;
            else left = rsz.x - lp.x - wNow;
            chipLeft = (left + wNow / 2) < rsz.x / 2;
            setCollapsed(true);
        } catch (Throwable ig) { }
    }

    /** 靠边收起 / 展开（收起=细把手贴边，点一下展开）。 */
    private void setCollapsed(boolean c) {
        if (chip == null || lp == null) return;
        collapsed = c;
        try {
            lp.width = c ? Ui.dp(this, 22) : Ui.dp(this, 52);
            lp.height = c ? Ui.dp(this, 64) : Ui.dp(this, 52);
            lp.x = c ? 0 : Ui.dp(this, 12);
            lp.gravity = (chipLeft ? Gravity.LEFT : Gravity.RIGHT) | Gravity.TOP;
            if (chipMark != null) chipMark.setVisibility(c ? View.GONE : View.VISIBLE);
            if (chipDot != null) chipDot.setVisibility(c ? View.VISIBLE : View.GONE);
            if (chipGlyph != null) chipGlyph.setVisibility(View.GONE);
            if (c) {
                chip.setBackground(Ui.pressableStroked(this, 0xF21D2126, Ui.CARD_LINE, 0x40FFFFFF, 11));
            } else {
                paintChip();
            }
            wm.updateViewLayout(chip, lp);
        } catch (Throwable ig) { }
    }

    private void setText(String t, int color) {
        if (chip == null) return;
        if (chipMark != null) chipMark.setVisibility(View.GONE);
        if (chipGlyph != null) {
            chipGlyph.setVisibility(View.VISIBLE);
            chipGlyph.setText(t);
            chipGlyph.setTextColor(color);
        }
        chip.setBackground(Ui.pressable(this, 0xF21D2126, 0x40FFFFFF, 16));
    }

    // ---------------------------------------------------------------- 试一下面板

    private void openTestPanel() {
        if (panelAdded) {
            closeTestPanel();
            return;
        }
        TestBench.appendLog(this, "chip_debug.txt", "openTestPanel");
        Ui.bg(new Runnable() {
            public void run() {
                final List<String> tmpls = Assets.namesTmpl();
                final List<String> ptss = Assets.namesPts();
                final List<String> ops = loadOpNames();
                h.post(new Runnable() {
                    public void run() { buildTestPanel(tmpls, ptss, ops); }
                });
            }
        }, null);
    }

    private void buildTestPanel(List<String> tmpls, List<String> ptss, List<String> ops) {
        try {
            pTmpls = tmpls;
            pPtss = ptss;
            pOps = ops;
            panelView = 0;
            LinearLayout col = Ui.col(this);
            col.setBackground(Ui.strokedRound(this, 0xFF14171A, Ui.CARD_LINE, 14));
            col.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 4));
            LinearLayout head = Ui.row(this);
            head.setGravity(Gravity.CENTER_VERTICAL);
            View dot = new View(this);
            dot.setBackground(Ui.round(this, Ui.ACCENT, 4));
            LinearLayout.LayoutParams dl = new LinearLayout.LayoutParams(Ui.dp(this, 8), Ui.dp(this, 8));
            dl.rightMargin = Ui.dp(this, 8);
            head.addView(dot, dl);
            head.addView(Ui.tv(this, "编辑悬浮窗", 14, Ui.TEXT, true),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView x = Ui.btnFlat(this, "\u2715", Ui.DIM);
            x.setMinWidth(Ui.dp(this, 44));
            x.setMinHeight(Ui.dp(this, 44));
            x.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { closeTestPanelAnimated(); }
            });
            head.addView(x);
            col.addView(head);
            panelResult = Ui.mono(this, "取图→模板 · 抓帧→帧库 · 取色→实时色值", 11, Ui.DIM);
            panelResult.setSingleLine(false);
            panelResult.setPadding(0, Ui.dp(this, 1), 0, Ui.dp(this, 4));
            col.addView(panelResult);
            panelScroll = new ScrollView(this);
            panelScroll.setVerticalScrollBarEnabled(false);
            panelScroll.addView(buildView());
            col.addView(panelScroll, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    Math.min(Ui.dp(this, 280), (int) (getResources().getDisplayMetrics().heightPixels * 0.36f))));
            col.addView(Ui.divider(this));
            LinearLayout bts = Ui.row(this);
            bts.setGravity(Gravity.CENTER);
            bts.setPadding(0, Ui.dp(this, 4), 0, 0);
            TextView hide = Ui.btnFlat(this, "靠边收起", Ui.DIM);
            hide.setMinHeight(Ui.dp(this, 44));
            hide.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { setCollapsed(true); closeTestPanelAnimated(); }
            });
            bts.addView(hide);
            col.addView(bts);
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams plp = new WindowManager.LayoutParams(
                    Ui.dp(this, 250),
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                    PixelFormat.TRANSLUCENT);
            plp.gravity = Gravity.TOP | Gravity.LEFT;
            Point rsz = new Point();
            wm.getDefaultDisplay().getRealSize(rsz);
            int[] pp = panelPosFor(rsz);
            plp.x = pp[0];
            plp.y = pp[1];
            hideChipNow();                                      /* ① 浮标立即隐去（此后所有窗口操作都在不可见期完成） */
            if (lp != null) {                                   /* ② 浮标锚点=窗口顶（先定锚，再算枢轴） */
                lp.y = pp[1];
                lp.gravity = (chipLeft ? Gravity.LEFT : Gravity.RIGHT) | Gravity.TOP;
                if (added && chip != null) {
                    try { wm.updateViewLayout(chip, lp); } catch (Throwable ig) { }
                }
            }
            int panelW = Ui.dp(this, 250);
            int chipW = chip != null && chip.getWidth() > 0 ? chip.getWidth() : Ui.dp(this, 52);
            int chipX0 = lp != null ? lp.x : Ui.dp(this, 12);   /* v9.1 修正：右侧原误用面板 x（plp.x）→ 枢轴偏移 ~86dp */
            int ballCx = (chipLeft ? chipX0 : rsz.x - chipX0 - chipW) + chipW / 2;
            int ballCy = (lp != null ? clampI(lp.y, 0, Math.max(0, rsz.y - chipW)) : pp[1]) + chipW / 2;
            col.setPivotX(clampI(ballCx - plp.x, 0, panelW));
            col.setPivotY(clampI(ballCy - plp.y, 0, Ui.dp(this, 430)));
            col.setScaleX(0.6f);
            col.setScaleY(0.6f);
            col.setAlpha(0f);
            wm.addView(col, plp);
            testPanel = col;
            panelLp = plp;
            panelAdded = true;
            raiseChipAbove();                                   /* ③ 浮标提到面板之上（不可见期重挂，零闪烁） */
            col.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(160).start();
            attachPanelDrag(col, head, plp);
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "测试面板添加失败: " + t);
        }
    }

    /** 面板位置=浮标处（顶对齐浮标、同侧贴边、夹屏内）——观感为"浮标变成窗口"。 */
    private int[] panelPosFor(Point rsz) {
        int x = chipLeft ? Ui.dp(this, 12) : rsz.x - Ui.dp(this, 12) - Ui.dp(this, 250);
        int chipY = lp != null ? lp.y : (int) (rsz.y * 0.30f);
        int panelH = Math.min(Ui.dp(this, 430), rsz.y - Ui.dp(this, 96));
        int top = clampI(chipY, 0, Math.max(0, rsz.y - panelH - Ui.dp(this, 8)));
        return new int[]{x, top};
    }

    private void clampPanel(WindowManager.LayoutParams p) {
        try {
            Point rsz = new Point();
            wm.getDefaultDisplay().getRealSize(rsz);
            int w = testPanel != null && testPanel.getWidth() > 0 ? testPanel.getWidth() : Ui.dp(this, 250);
            int h = testPanel != null && testPanel.getHeight() > 0 ? testPanel.getHeight() : Ui.dp(this, 400);
            p.x = clampI(p.x, 0, Math.max(0, rsz.x - w));
            int maxY = rsz.y - Math.min(h, rsz.y - Ui.dp(this, 120));
            p.y = clampI(p.y, 0, Math.max(0, maxY));
        } catch (Throwable ig) { }
    }

    private void closeTestPanel() {
        if (panelAdded && testPanel != null) {
            try { wm.removeView(testPanel); } catch (Throwable ig) { }
        }
        if (closingPanel != null) {                 /* 关闭动画中（快速收场）：取消动画即快进——清帧/卸窗仍由链路异步完成（不回闪） */
            try { closingPanel.animate().cancel(); } catch (Throwable ig) { }
            closingPanel = null;
        }
        testPanel = null;
        panelResult = null;
        panelScroll = null;
        panelLp = null;
        panelAdded = false;
        syncChipVisibility();
    }

    /** 收起（v9.1）：窗口向浮标处反向形变（缩放 0.6 + 淡出 140ms，镜像展开）→ 清空内容、等帧提交后卸窗（消闪）→ 浮标淡入复现。
     *  消闪原理：卸窗瞬间合成器重置节点属性，若最后缓冲仍有内容即整帧回闪（v8.16 实锤帧 182）；
     *  故卸窗前先把窗口内容清空（缓冲变透明），并多等 ~4 帧（64ms）确保清帧已提交呈现后再移除。
     *  关闭期间交互入口以 closingPanel 为闸（快速收场走 closeTestPanel，取消动画即快进到清帧卸窗）。 */
    private void closeTestPanelAnimated() {
        if (!panelAdded || testPanel == null || panelLp == null) {
            closeTestPanel();
            return;
        }
        final LinearLayout v = testPanel;
        TestBench.appendLog(this, "chip_debug.txt", "closeTestPanelAnimated");
        final WindowManager.LayoutParams plp = panelLp;
        panelAdded = false;
        testPanel = null;
        panelResult = null;
        panelScroll = null;
        panelLp = null;
        closingPanel = v;                                   /* 链路持有；非空 = 正在关闭 */
        try {                                               /* 枢轴 = 浮标处（与展开同一算法） */
            Point rsz = new Point();
            wm.getDefaultDisplay().getRealSize(rsz);
            int cw = lp != null && lp.width > 0 ? lp.width : Ui.dp(this, 52);
            int ch = lp != null && lp.height > 0 ? lp.height : Ui.dp(this, 52);
            int cxx = lp != null ? lp.x : Ui.dp(this, 12);
            int ballCx = (chipLeft ? cxx : rsz.x - cxx - cw) + cw / 2;
            int ballCy = (lp != null ? clampI(lp.y, 0, Math.max(0, rsz.y - ch)) : plp.y) + ch / 2;
            v.setPivotX(clampI(ballCx - plp.x, 0, Ui.dp(this, 250)));
            v.setPivotY(clampI(ballCy - plp.y, 0, Ui.dp(this, 430)));
        } catch (Throwable ig) { }
        final boolean[] gone = new boolean[1];
        final Runnable finisher = new Runnable() {
            public void run() {
                if (gone[0]) return;
                gone[0] = true;
                if (closingPanel == v) closingPanel = null;
                try { wm.removeView(v); } catch (Throwable ig) { }
                TestBench.appendLog(CaptureChip.this, "chip_debug.txt", "closePanel removed");
                syncChipVisibility();                       /* 面板已消失：浮标恢复可点 */
            }
        };
        v.animate().scaleX(0.6f).scaleY(0.6f).alpha(0f).setDuration(140)
                .withEndAction(new Runnable() {
                    public void run() {
                        clearPanelFrame(v);                 /* ① 清空内容：窗口缓冲变透明（卸窗消闪的前提） */
                        v.postDelayed(finisher, 64);        /* ② 等清帧提交呈现（~4 帧）后再卸窗 */
                    }
                }).start();
        v.postDelayed(new Runnable() {                      /* 兜底：动画机制异常时也保证窗口被移除（无幽灵窗） */
            public void run() {
                if (gone[0]) return;
                clearPanelFrame(v);
                v.postDelayed(finisher, 64);
            }
        }, 600);
        h.postDelayed(new Runnable() {                      /* ③ 浮标复现：面板缩到一半时开始淡入（出现用动画） */
            public void run() {
                if (closingPanel != v || chip == null || !added || lp == null) return;
                chip.animate().cancel();
                chip.animate().alpha(1f).setDuration(120).start();
            }
        }, 50);
    }

    /** 清空面板窗口内容（缓冲变透明；异常静默）——卸窗消闪的前提。 */
    private void clearPanelFrame(LinearLayout v) {
        try {
            v.removeAllViews();
            v.setBackground(null);
            v.setAlpha(0f);
            v.invalidate();
        } catch (Throwable ig) { }
    }

    /** 立即隐去浮标（瞬变）——随后一切窗口操作都发生在不可见期，杜绝闪帧。 */
    private void hideChipNow() {
        if (chip == null || !added || lp == null) return;
        chip.animate().cancel();
        chip.setAlpha(0f);
        chip.setEnabled(false);
        if (lp.alpha != 1f) {
            lp.alpha = 1f;                                    /* 窗口层保持"可见"（SAW/豁免） */
            try { wm.updateViewLayout(chip, lp); } catch (Throwable ig) { }
        }
        if ((lp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0) {
            lp.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;   /* 展开期间不挡面板 */
            try { wm.updateViewLayout(chip, lp); } catch (Throwable ig) { }
        }
    }

    /** 浮标显隐同步：面板在=立即隐藏且不可点；面板无=淡入出现。
     *  规则：消失瞬变（移除/隐藏不产生回闪），出现才用动画。 */
    private void syncChipVisibility() {
        if (chip == null || !added || lp == null) return;
        if (lp.alpha != 1f) setChipAlpha(1f);                 /* 窗口层保持"可见"（SAW/豁免） */
        boolean wantNoTouch = panelAdded;                     /* 面板展开期间：浮标窗口禁触，不挡面板 */
        boolean hasNoTouch = (lp.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0;
        if (wantNoTouch != hasNoTouch) {
            if (wantNoTouch) lp.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
            else lp.flags &= ~WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
            try { wm.updateViewLayout(chip, lp); } catch (Throwable ig) { }
        }
        chip.animate().cancel();
        chip.setEnabled(!panelAdded);
        if (panelAdded) {
            chip.setAlpha(0f);                                /* 立即隐藏 */
        } else {
            chip.animate().alpha(1f).setDuration(140).start(); /* 淡入出现 */
        }
    }

    /** 把浮标窗口提到最上层（此刻浮标不可见，重挂零闪烁）——收起时浮标淡入才能盖过窗口（消闪）。 */
    private void raiseChipAbove() {
        if (chip == null || !added || lp == null) return;
        try {
            wm.removeView(chip);
            wm.addView(chip, lp);
        } catch (Throwable ig) { }
    }

    /** 头部拖动 = 移动整体（展开窗口 + 隐藏的浮标锚点）；松手吸附到近边。 */
    private void attachPanelDrag(final View panel, final View head, final WindowManager.LayoutParams plp) {
        final float[] pd = new float[4];
        final boolean[] moved = new boolean[1];
        head.setOnTouchListener(new View.OnTouchListener() {
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        pd[0] = e.getRawX();
                        pd[1] = e.getRawY();
                        pd[2] = plp.x;
                        pd[3] = plp.y;
                        moved[0] = false;
                        return true;
                    case MotionEvent.ACTION_MOVE: {
                        float dx = e.getRawX() - pd[0];
                        float dy = e.getRawY() - pd[1];
                        if (Math.abs(dx) + Math.abs(dy) > Ui.dp(CaptureChip.this, 8)) moved[0] = true;
                        if (moved[0]) {
                            plp.x = (int) (pd[2] + dx);
                            plp.y = (int) (pd[3] + dy);
                            clampPanel(plp);
                            try { wm.updateViewLayout(panel, plp); } catch (Throwable ig) { }
                        }
                        return true;
                    }
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (moved[0]) snapPanelPair(plp, panel);
                        return true;
                }
                return false;
            }
        });
    }

    /** 松手：整体吸附近边（窗口贴边、浮标锚点同步到窗口顶角）。 */
    private void snapPanelPair(WindowManager.LayoutParams plp, View panel) {
        try {
            Point rsz = new Point();
            wm.getDefaultDisplay().getRealSize(rsz);
            int panelW = Ui.dp(this, 250);
            chipLeft = (plp.x + panelW / 2) < rsz.x / 2;
            plp.x = chipLeft ? Ui.dp(this, 12) : rsz.x - Ui.dp(this, 12) - panelW;
            clampPanel(plp);
            try { wm.updateViewLayout(panel, plp); } catch (Throwable ig) { }
            if (lp != null) {
                lp.x = Ui.dp(this, 12);
                lp.y = plp.y;
                lp.gravity = (chipLeft ? Gravity.LEFT : Gravity.RIGHT) | Gravity.TOP;
                if (added && chip != null) {
                    try { wm.updateViewLayout(chip, lp); } catch (Throwable ig) { }
                }
                saveChipPos();
            }
        } catch (Throwable ig) { }
    }

    // ---------------------------------------------------------------- 面板视图（两级 + 报告）

    private LinearLayout buildView() {
        if (panelView == 1) return buildAssetsView();
        if (panelView == 2) return buildOpsView();
        return buildRootView();
    }

    private void gotoView(int v) {
        panelView = v;
        if (panelScroll == null) return;
        panelScroll.removeAllViews();
        panelScroll.addView(buildView());
        if (panelResult != null && v != 3) {                 /* 状态行=各视图的一句话说明（结果会临时覆盖） */
            panelResult.setTextColor(Ui.DIM);
            panelResult.setText(v == 0 ? "取图→模板 · 抓帧→帧库 · 取色→实时色值"
                    : v == 1 ? "点 [试一下] 在当前屏幕上实找一次"
                    : "点 [干跑] 评估操作视觉步（不执行触摸）");
        }
    }

    /** 主视图：三个主操作 + 最近 + 两个钻入入口。 */
    private LinearLayout buildRootView() {
        LinearLayout root = Ui.col(this);
        LinearLayout acts = Ui.row(this);
        TextView capB = actBtn("取图", new View.OnClickListener() {
            public void onClick(View v) { doCapture(1); }
        });
        LinearLayout.LayoutParams a1 = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        a1.rightMargin = Ui.dp(this, 8);
        acts.addView(capB, a1);
        TextView grabB = actBtn("抓帧", new View.OnClickListener() {
            public void onClick(View v) { doCapture(0); }
        });
        LinearLayout.LayoutParams a2 = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        a2.rightMargin = Ui.dp(this, 8);
        acts.addView(grabB, a2);
        TextView pcB = actBtn("取色", new View.OnClickListener() {
            public void onClick(View v) { startPanelColor(); }
        });
        acts.addView(pcB, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        acts.setPadding(0, Ui.dp(this, 2), 0, Ui.dp(this, 4));
        root.addView(acts);
        root.addView(navRow("测试素材", pTmpls.size() + "模板 · " + pPtss.size() + "点集",
                new View.OnClickListener() {
                    public void onClick(View v) { gotoView(1); }
                }));
        root.addView(navRow("脚本干跑", pOps.size() + "操作",
                new View.OnClickListener() {
                    public void onClick(View v) { gotoView(2); }
                }));
        if (!recentAssets.isEmpty()) {
            root.addView(secLabel("最近测过"));
            root.addView(recentChipsRow());
        }
        return root;
    }

    /** 最近测过：一行小胶囊（点即重测）。 */
    private HorizontalScrollView recentChipsRow() {
        HorizontalScrollView hs = new HorizontalScrollView(this);
        hs.setHorizontalScrollBarEnabled(false);
        LinearLayout strip = Ui.row(this);
        for (String key : recentAssets) {
            int bar = key.lastIndexOf('|');
            if (bar <= 0) continue;
            final String nm = key.substring(0, bar);
            final boolean pts = key.endsWith("|1");
            TextView c = Ui.mono(this, nm, 11, Ui.TEXT);
            c.setPadding(Ui.dp(this, 10), Ui.dp(this, 6), Ui.dp(this, 10), Ui.dp(this, 6));
            c.setBackground(Ui.pressable(this, 0x40242A31, 0x40FFFFFF, 8));
            c.setClickable(true);
            LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            cl.rightMargin = Ui.dp(this, 6);
            c.setLayoutParams(cl);
            c.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { runQuick(nm, pts); }
            });
            strip.addView(c);
        }
        hs.addView(strip);
        return hs;
    }

    /** 测试素材视图：模板/点集聚焦列表（行内 [试一下] + 行内结果）。 */
    private LinearLayout buildAssetsView() {
        LinearLayout v = Ui.col(this);
        v.addView(backRow("测试素材", 0));
        qrowViews.clear();
        if (pTmpls.isEmpty() && pPtss.isEmpty()) {
            TextView e = Ui.tv(this, "素材库为空\n先在编辑台创建模板或点集", 12, Ui.WARN, false);
            e.setGravity(Gravity.CENTER);
            e.setSingleLine(false);
            e.setPadding(0, Ui.dp(this, 24), 0, Ui.dp(this, 24));
            v.addView(e);
            return v;
        }
        if (!pTmpls.isEmpty()) {
            v.addView(secCount("模板", pTmpls.size()));
            v.addView(threshRow());                        /* 试查阈值：只影响模板试查（点集容差随点集） */
            for (String n : pTmpls) v.addView(assetRow(n, false));
        }
        if (!pPtss.isEmpty()) {
            v.addView(secCount("点集", pPtss.size()));
            for (String n : pPtss) v.addView(assetRow(n, true));
        }
        return v;
    }

    /** 脚本干跑视图：操作列表。 */
    private LinearLayout buildOpsView() {
        LinearLayout v = Ui.col(this);
        v.addView(backRow("脚本干跑"));
        if (pOps.isEmpty()) {
            v.addView(Ui.tv(this, "没有操作（先在编辑台建操作）", 12, Ui.WARN, false));
            return v;
        }
        v.addView(secCount("操作", pOps.size()));
        for (String n : pOps) v.addView(opRow(n));
        return v;
    }

    private LinearLayout backRow(String title) {
        return backRow(title, 0);
    }

    private LinearLayout backRow(String title, final int backTo) {
        LinearLayout row = Ui.row(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, 0, 0, Ui.dp(this, 2));
        TextView back = Ui.btnFlat(this, "\u2039 返回", Ui.ACCENT);
        back.setMinHeight(Ui.dp(this, 48));
        back.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { gotoView(backTo); }
        });
        row.addView(back);
        TextView tt = Ui.tv(this, title, 12, Ui.DIM, true);
        tt.setGravity(Gravity.CENTER);
        row.addView(tt, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        View pad = new View(this);                       /* 与返回按钮同宽占位：标题视觉居中 */
        row.addView(pad, new LinearLayout.LayoutParams(Ui.dp(this, 56), 1));
        return row;
    }

    private TextView secLabel(String s) {
        TextView t = Ui.tv(this, s, 11, Ui.DIM, true);
        t.setPadding(Ui.dp(this, 4), Ui.dp(this, 6), 0, Ui.dp(this, 2));
        return t;
    }

    /** 节头：标题 + 计数。 */
    private TextView secCount(String title, int n) {
        TextView t = Ui.tv(this, title + "（" + n + "）", 11, Ui.DIM, true);
        t.setPadding(Ui.dp(this, 4), Ui.dp(this, 8), 0, Ui.dp(this, 2));
        return t;
    }

    /** 试查阈值行（只影响模板试查；步骤用的值在编辑器里设）。 */
    private LinearLayout threshRow() {
        LinearLayout tr = Ui.row(this);
        tr.setGravity(Gravity.CENTER_VERTICAL);
        tr.setPadding(Ui.dp(this, 12), 0, 0, Ui.dp(this, 4));
        tr.addView(Ui.tv(this, "试查阈值", 11, Ui.DIM, false),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        final TextView qv = Ui.mono(this, String.valueOf(quickThresh), 12, Ui.TEXT);
        qv.setMinWidth(Ui.dp(this, 28));
        qv.setGravity(Gravity.CENTER);
        tr.addView(qsBtn("−10", -10, qv));
        tr.addView(qsBtn("−1", -1, qv));
        tr.addView(qv);
        tr.addView(qsBtn("＋1", 1, qv));
        tr.addView(qsBtn("＋10", 10, qv));
        return tr;
    }

    /** 行内小胶囊（动作/状态）：描边随状态着色。 */
    private TextView miniChip(String label, int color, int border) {
        TextView t = Ui.mono(this, label, 11, color);
        t.setPadding(Ui.dp(this, 9), Ui.dp(this, 6), Ui.dp(this, 9), Ui.dp(this, 6));
        t.setBackground(Ui.pressableStroked(this, 0x00000000, border, 0x40FFFFFF, 8));
        return t;
    }

    private static String akey(String name, boolean isPts) { return name + "|" + (isPts ? 1 : 0); }

    private void applyAssetState(TextView act, TextView sub, String[] st) {
        int c = "o".equals(st[1]) ? Ui.OK : "w".equals(st[1]) ? Ui.WARN : "a".equals(st[1]) ? Ui.ACCENT : Ui.DIM;
        act.setText(st[0]);
        act.setTextColor(c);
        act.setBackground(Ui.pressableStroked(this, 0x00000000, c, 0x40FFFFFF, 8));
        if (st[2] != null && st[2].length() > 0) {
            sub.setText(st[2]);
            sub.setTextColor("o".equals(st[1]) ? Ui.OK : "w".equals(st[1]) ? Ui.WARN : Ui.FAINT);
            sub.setVisibility(View.VISIBLE);
        } else {
            sub.setVisibility(View.GONE);
        }
    }

    /** 行内状态：胶囊文案 + 色键（a/o/w/d）+ 明细行。 */
    private void setAssetState(String name, boolean isPts, String chip, String colorKey, String sub) {
        String k = akey(name, isPts);
        String[] st = new String[]{chip, colorKey, sub == null ? "" : sub};
        qres.put(k, st);
        TextView[] vs = qrowViews.get(k);
        if (vs != null) applyAssetState(vs[0], vs[1], st);
    }

    private TextView actBtn(String label, View.OnClickListener l) {
        TextView t = Ui.tv(this, label, 13, Ui.TEXT, true);
        t.setGravity(Gravity.CENTER);
        t.setMinHeight(Ui.dp(this, 46));
        t.setBackground(Ui.pressableStroked(this, 0x00000000, Ui.CARD_LINE, 0x40FFFFFF, 12));
        t.setClickable(true);
        t.setFocusable(true);
        t.setOnClickListener(l);
        return t;
    }

    private LinearLayout navRow(String title, String count, View.OnClickListener l) {
        LinearLayout row = Ui.row(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(Ui.dp(this, 10), 0, Ui.dp(this, 8), 0);
        row.setMinimumHeight(Ui.dp(this, 46));
        row.setBackground(Ui.pressable(this, 0x00000000, 0x40FFFFFF, 10));
        row.setClickable(true);
        row.setFocusable(true);
        row.setOnClickListener(l);
        row.addView(Ui.tv(this, title, 13, Ui.TEXT, true),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(Ui.tv(this, count, 11, Ui.DIM, false));
        TextView chev = Ui.tv(this, "\u203A", 15, Ui.FAINT, false);
        chev.setPadding(Ui.dp(this, 6), 0, 0, 0);
        row.addView(chev);
        return row;
    }

    /** 测试阈值步进按钮（紧凑：面板宽 250dp，5 键 + 标签须塞下）。 */
    private TextView qsBtn(String label, final int delta, final TextView val) {
        TextView b = Ui.mono(this, label, 12, Ui.ACCENT);
        b.setMinWidth(Ui.dp(this, 32));
        b.setGravity(Gravity.CENTER);
        b.setPadding(Ui.dp(this, 2), Ui.dp(this, 6), Ui.dp(this, 2), Ui.dp(this, 6));
        b.setBackground(Ui.pressableStroked(this, 0x00000000, Ui.CARD_LINE, 0x40FFFFFF, 8));
        b.setClickable(true);
        b.setFocusable(true);
        b.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                quickThresh = clampI(quickThresh + delta, 0, 255);
                val.setText(String.valueOf(quickThresh));
            }
        });
        return b;
    }

    /** 素材行：名称 + 行内 [试一下]/结果胶囊 + 结果明细行（有结果才显示）。 */
    private LinearLayout assetRow(final String name, final boolean isPts) {
        LinearLayout box = Ui.col(this);
        box.setBackground(Ui.pressable(this, 0x00000000, 0x40FFFFFF, 10));
        box.setPadding(Ui.dp(this, 12), 0, Ui.dp(this, 8), 0);
        box.setClickable(true);
        box.setFocusable(true);
        box.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runQuick(name, isPts); }
        });
        LinearLayout line1 = Ui.row(this);
        line1.setGravity(Gravity.CENTER_VERTICAL);
        line1.setMinimumHeight(Ui.dp(this, 44));
        TextView nm = Ui.mono(this, name, 13, Ui.TEXT);
        nm.setSingleLine(true);
        nm.setEllipsize(TextUtils.TruncateAt.END);
        line1.addView(nm, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView act = miniChip("试一下", Ui.ACCENT, Ui.CARD_LINE);
        act.setClickable(true);
        act.setFocusable(false);
        act.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runQuick(name, isPts); }
        });
        line1.addView(act);
        box.addView(line1);
        TextView sub = Ui.mono(this, "", 11, Ui.FAINT);
        sub.setSingleLine(true);
        sub.setEllipsize(TextUtils.TruncateAt.END);
        sub.setPadding(0, 0, 0, Ui.dp(this, 6));
        sub.setVisibility(View.GONE);
        box.addView(sub);
        String k = akey(name, isPts);
        qrowViews.put(k, new TextView[]{act, sub});
        String[] st = qres.get(k);
        if (st != null) applyAssetState(act, sub, st);
        return box;
    }

    /** 操作行：名称 + 行尾 [干跑] 胶囊。 */
    private LinearLayout opRow(final String name) {
        LinearLayout row = Ui.row(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(Ui.dp(this, 12), 0, Ui.dp(this, 8), 0);
        row.setMinimumHeight(Ui.dp(this, 44));
        row.setBackground(Ui.pressable(this, 0x00000000, 0x40FFFFFF, 10));
        row.setClickable(true);
        row.setFocusable(true);
        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runDry(name); }
        });
        TextView nm = Ui.mono(this, name, 13, Ui.TEXT);
        nm.setSingleLine(true);
        nm.setEllipsize(TextUtils.TruncateAt.END);
        row.addView(nm, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView act = miniChip("干跑", Ui.ACCENT, Ui.CARD_LINE);
        act.setClickable(true);
        act.setFocusable(false);
        act.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { runDry(name); }
        });
        row.addView(act);
        return row;
    }

    /** 面板取色：关面板 → 透明浮标（SAW 在位）→ 全屏遮罩吞一次触摸。 */
    private void startPanelColor() {
        if (closingPanel != null) return;              /* 关闭动画中：忽略 */
        pickKind = "color";
        closeTestPanel();
        setChipAlpha(0f);
        addPickOverlay();
        TestBench.appendLog(this, "chip_debug.txt", "panel color-pick");
    }

    private void touchRecent(String name, boolean isPts) {
        String key = name + "|" + (isPts ? 1 : 0);
        recentAssets.remove(key);
        recentAssets.add(0, key);
        while (recentAssets.size() > 3) recentAssets.remove(recentAssets.size() - 1);
    }

    /** 干跑报告：内容区替换为逐行结果（[返回] 回操作列表）。 */
    private void showReport(DryRun.Report rep) {
        if (panelScroll == null) return;
        panelView = 3;
        LinearLayout col = Ui.col(this);
        col.addView(backRow("干跑报告", 2));
        for (DryRun.Line ln : rep.lines) {
            TextView t = Ui.mono(this, "· " + ln.text, 11, ln.hit ? Ui.OK : Ui.WARN);
            t.setSingleLine(false);
            t.setPadding(Ui.dp(this, 4), Ui.dp(this, 3), 0, Ui.dp(this, 3));
            col.addView(t);
        }
        panelScroll.removeAllViews();
        panelScroll.addView(col);
    }

    private static List<String> loadOpNames() {
        List<String> ls = new ArrayList<String>();
        Su.R tr = Su.run("cat " + Ops.PATH + " 2>/dev/null");
        if (!tr.ok() || tr.out == null) return ls;
        Ops.File of = Ops.File.parse(tr.out);
        if (of == null) return ls;
        for (Ops.Op o : of.ops) ls.add(o.name);
        return ls;
    }

    /** 区域名 → 逻辑矩形 (x,y,w,h)（口径同 OpsPage.regionRect；圆取外接矩形）。 */
    private static Map<String, int[]> loadRegionRects() {
        Map<String, int[]> m = new HashMap<String, int[]>();
        Su.R tr = Su.run("cat " + Regions.PATH + " 2>/dev/null");
        if (!tr.ok() || tr.out == null) return m;
        Regions.File rf = Regions.File.parse(tr.out);
        if (rf == null) return m;
        for (Regions.Region r : rf.regions) {
            if (r.type == 0) m.put(r.id, new int[]{r.a1, r.a2, r.a3, r.a4});
            else m.put(r.id, new int[]{r.a1 - r.a3, r.a2 - r.a3, r.a3 * 2, r.a3 * 2});
        }
        return m;
    }

    /** 脚本干跑：抓当前屏幕 → 评估操作各视觉步（找图/找色）命中；不执行任何触摸。 */
    private void runDry(final String opName) {
        if (capturing || closingPanel != null) return;
        capturing = true;
        TestBench.appendLog(this, "chip_debug.txt", "runDry " + opName);
        if (panelResult != null) panelResult.setText("干跑中… " + opName);
        setChipAlpha(0f);
        if (panelAdded && testPanel != null) {                 /* 抓帧排除：alpha（非 GONE，无重布局） */
            testPanel.animate().cancel();
            testPanel.setAlpha(0f);
        }
        h.postDelayed(new Runnable() {
            public void run() {
                new Thread(new Runnable() {
                    public void run() {
                        DryRun.Report rep = null;
                        String err = null;
                        int fw = 0, fh = 0;
                        try {
                            Capture.Shot s = Capture.grab();
                            fw = s.w;
                            fh = s.h;
                            restorePanelAlpha();                   /* 抓完即恢复 */
                            Su.R tr = Su.run("cat " + Ops.PATH + " 2>/dev/null");
                            Ops.File of = tr.ok() && tr.out != null ? Ops.File.parse(tr.out) : null;
                            Ops.Op op = of == null ? null : of.find(opName);
                            if (op == null) err = "操作读不到：" + opName;
                            else rep = DryRun.run(s.rgba, s.w, s.h, s.rot, op, loadRegionRects());
                        } catch (Exception ex) {
                            err = ex.getMessage();
                            restorePanelAlpha();
                        }
                        final DryRun.Report frep = rep;
                        final String ferr = err;
                        final int ffw = fw, ffh = fh;
                        h.post(new Runnable() {
                            public void run() {
                                capturing = false;
                                syncChipVisibility();
                                if (frep == null) {
                                    if (panelResult != null) panelResult.setText("干跑失败：" + ferr);
                                    showResultToast("干跑失败：" + ferr, false);
                                    return;
                                }
                                TestBench.appendLog(CaptureChip.this, "chip_debug.txt", "runDry result " + frep.summary);
                                if (panelResult != null) panelResult.setText(frep.summary);
                                showResultToast(frep.summary, frep.hits > 0);
                                showReport(frep);
                                for (DryRun.Line ln : frep.lines) {
                                    if (ln.hit) showMarker(ln.x, ln.y, ln.w, ln.h, ffw, ffh);
                                }
                            }
                        });
                    }
                }, "chip-dry").start();
            }
        }, 120);
    }

    /** 面板素材行：当场抓帧 → QuickTest（在当前屏幕上实时找） → 结果行 + 屏幕标记。 */
    /** 抓帧后立即恢复面板可见（alpha 恢复，无窗口重布局；计算期间不再隐藏）。 */
    private void restorePanelAlpha() {
        h.post(new Runnable() {
            public void run() {
                if (panelAdded && testPanel != null) {
                    testPanel.animate().cancel();
                    testPanel.animate().alpha(1f).setDuration(120).start();
                }
            }
        });
    }

    private void runQuick(final String name, final boolean isPts) {
        if (capturing || closingPanel != null) return;
        capturing = true;
        TestBench.appendLog(this, "chip_debug.txt", "runQuick " + name + " pts=" + isPts);
        touchRecent(name, isPts);
        setAssetState(name, isPts, "试查中", "d", "");
        setChipAlpha(0f);
        if (panelAdded && testPanel != null) {                 /* 抓帧排除：alpha（非 GONE，无重布局） */
            testPanel.animate().cancel();
            testPanel.setAlpha(0f);
        }
        h.postDelayed(new Runnable() {
            public void run() {
                new Thread(new Runnable() {
                    public void run() {
                        String text;
                        boolean hit = false, err = false;
                        int hx = 0, hy = 0, hw = 0, hh = 0, fw = 0, fh = 0;
                        try {
                            Capture.Shot s = Capture.grab();
                            fw = s.w;
                            fh = s.h;
                            restorePanelAlpha();                   /* 抓完即恢复 */
                            QuickTest.Result r = QuickTest.run(s.rgba, s.w, s.h, s.rot, name, isPts, quickThresh);
                            hit = r.hit;
                            hx = r.x;
                            hy = r.y;
                            hw = r.w;
                            hh = r.h;
                            text = r.text;
                        } catch (Exception ex) {
                            err = true;
                            restorePanelAlpha();
                            text = "抓帧失败：" + ex.getMessage();
                        }
                        final String ft = text;
                        final boolean fhit = hit, ferr = err;
                        final int fx = hx, fy = hy, fwd = hw, fhd = hh, ffw = fw, ffh = fh;
                        h.post(new Runnable() {
                            public void run() {
                                capturing = false;
                                TestBench.appendLog(CaptureChip.this, "chip_debug.txt", "runQuick result " + name + "：" + ft);
                                syncChipVisibility();
                                if (ferr) {
                                    setAssetState(name, isPts, "✗ 失败", "w", ft);
                                } else if (fhit) {
                                    setAssetState(name, isPts, "✓ 命中", "o", ft);
                                } else {
                                    setAssetState(name, isPts, "✗ 未命中", "w", ft);
                                }
                                if (panelResult != null) {
                                    panelResult.setText(name + "：" + ft);
                                    panelResult.setTextColor(ferr || !fhit ? Ui.WARN : Ui.OK);
                                }
                                if (fhit) showMarker(fx, fy, fwd, fhd, ffw, ffh);
                            }
                        });
                    }
                }, "chip-test").start();
            }
        }, 100);
    }

    /** 顶部结果条（2.6s 自动收起）。 */
    private void showResultToast(String text, boolean hit) {
        try {
            final TextView tv = new TextView(this);
            tv.setText(text);
            tv.setTextSize(13);
            tv.setTextColor(hit ? Ui.OK : Ui.WARN);
            tv.setBackground(Ui.strokedRound(this, 0xF214171A, hit ? Ui.OK : Ui.CARD_LINE, 8));
            tv.setPadding(Ui.dp(this, 14), Ui.dp(this, 10), Ui.dp(this, 14), Ui.dp(this, 10));
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams lp2 = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE,
                    PixelFormat.TRANSLUCENT);
            lp2.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            lp2.y = Ui.dp(this, 110);
            wm.addView(tv, lp2);
            h.postDelayed(new Runnable() {
                public void run() {
                    try { wm.removeView(tv); } catch (Throwable ig) { }
                }
            }, 2600);
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "结果条失败: " + t);
        }
    }

    /** 命中框标在屏幕上（帧坐标 → 屏幕像素；2.6s 自动收起）。 */
    private void showMarker(int fx, int fy, int fw2, int fh2, int frameW, int frameH) {
        if (frameW <= 0 || frameH <= 0) return;
        try {
            Point rsz = new Point();
            wm.getDefaultDisplay().getRealSize(rsz);
            float sx = (float) rsz.x / frameW;
            float sy = (float) rsz.y / frameH;
            final View mk = new View(this);
            mk.setBackground(Ui.strokedRound(this, 0x2200FF88, Ui.OK, 4));
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams lp3 = new WindowManager.LayoutParams(
                    Math.max(Ui.dp(this, 16), (int) (fw2 * sx)),
                    Math.max(Ui.dp(this, 16), (int) (fh2 * sy)),
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                    PixelFormat.TRANSLUCENT);
            lp3.gravity = Gravity.TOP | Gravity.LEFT;
            lp3.x = (int) (fx * sx);
            lp3.y = (int) (fy * sy);
            wm.addView(mk, lp3);
            h.postDelayed(new Runnable() {
                public void run() {
                    try { wm.removeView(mk); } catch (Throwable ig) { }
                }
            }, 2600);
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "命中标记失败: " + t);
        }
    }

    // ---------------------------------------------------------------- 屏幕取点（v7.2）

    private void addPickOverlay() {
        try {
            if (pickAdded) return;
            if (wm == null) wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            FrameLayout root = new FrameLayout(this);
            root.setBackgroundColor(0x1A000000);
            TextView hint = Ui.tv(this, "屏幕取色：点一下屏幕取色值（不存帧）", 13, Ui.TEXT, true);
            hint.setBackground(Ui.strokedRound(this, 0xF214171A, Ui.CARD_LINE, 10));
            hint.setPadding(Ui.dp(this, 14), Ui.dp(this, 10), Ui.dp(this, 14), Ui.dp(this, 10));
            FrameLayout.LayoutParams hl = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
            hl.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            hl.bottomMargin = Ui.dp(this, 120);
            root.addView(hint, hl);
            TextView cancel = Ui.btnSmall(this, "取消");
            FrameLayout.LayoutParams cl = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
            cl.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            cl.topMargin = Ui.dp(this, 110);
            cancel.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { hideAll("pick-cancel"); }
            });
            root.addView(cancel, cl);
            root.setOnTouchListener(new View.OnTouchListener() {
                public boolean onTouch(View v, MotionEvent e) {
                    switch (e.getActionMasked()) {
                        case MotionEvent.ACTION_DOWN:
                            pickDownX = e.getRawX();
                            pickDownY = e.getRawY();
                            pickMoved = false;
                            return true;
                        case MotionEvent.ACTION_MOVE:
                            if (Math.abs(e.getRawX() - pickDownX) + Math.abs(e.getRawY() - pickDownY)
                                    > Ui.dp(CaptureChip.this, 24)) pickMoved = true;
                            return true;
                        case MotionEvent.ACTION_UP:
                            if (!pickMoved) doPick((int) e.getRawX(), (int) e.getRawY());
                            return true;
                    }
                    return true;
                }
            });
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams plp = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.MATCH_PARENT,
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                    PixelFormat.TRANSLUCENT);
            plp.gravity = Gravity.TOP | Gravity.LEFT;
            wm.addView(root, plp);
            pickOverlay = root;
            pickAdded = true;
            TestBench.appendLog(this, "chip_debug.txt", "pickOverlay added");
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "取点遮罩添加失败: " + t);
        }
    }

    private void removePickOverlay() {
        if (pickAdded && pickOverlay != null) {
            try { wm.removeView(pickOverlay); } catch (Throwable ig) { }
        }
        pickOverlay = null;
        pickAdded = false;
    }

    /** 屏幕取色（实时）：摘遮罩 → 抓帧读该点色值 → 浮动结果卡（不存帧、不回编辑器）。 */
    private void doPick(final int sx, final int sy) {
        if (capturing) return;
        capturing = true;
        TestBench.appendLog(this, "chip_debug.txt", "doPickColor " + sx + "," + sy);
        removePickOverlay();
        setChipAlpha(0f);
        if (panelAdded && testPanel != null) testPanel.setVisibility(View.GONE);
        h.postDelayed(new Runnable() {
            public void run() {
                new Thread(new Runnable() {
                    public void run() {
                        String err = null;
                        int rgb = -1, lx = 0, ly = 0, px2 = 0, py2 = 0;
                        try {
                            Capture.Shot s = Capture.grab();
                            px2 = clampI(sx, 0, s.w - 1);
                            py2 = clampI(sy, 0, s.h - 1);
                            int o = (py2 * s.w + px2) * 4;
                            rgb = ((s.rgba[o] & 0xFF) << 16) | ((s.rgba[o + 1] & 0xFF) << 8)
                                    | (s.rgba[o + 2] & 0xFF);
                            int[] lg = new int[2];
                            Matcher.frameToLogic(s.rot, s.w, s.h, px2, py2, lg);
                            lx = lg[0];
                            ly = lg[1];
                            FrameStore.flashShot = s;
                            FrameStore.flashTs = System.currentTimeMillis();
                        } catch (Exception ex) {
                            err = ex.getMessage();
                        }
                        final String ferr = err;
                        final int frgb = rgb, flx = lx, fly = ly, fpx = px2, fpy = py2;
                        h.post(new Runnable() {
                            public void run() {
                                capturing = false;
                                syncChipVisibility();
                                if (panelAdded && testPanel != null) testPanel.setVisibility(View.VISIBLE);
                                if (ferr != null) {
                                    showResultToast("取色失败：" + ferr, false);
                                    return;
                                }
                                TestBench.appendLog(CaptureChip.this, "chip_debug.txt",
                                        "doPickColor ok #" + Integer.toHexString(frgb) + " @" + flx + "," + fly);
                                VisualPage.stashScreenPick(fpx, fpy);
                                showColorCard(frgb, flx, fly);
                            }
                        });
                    }
                }, "chip-color").start();
            }
        }, 120);
    }

    private static int clampI(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /** 取色结果卡（顶部浮出：色块 + #RRGGBB + 逻辑坐标；4s 自动收起）。 */
    private void showColorCard(int rgb, int lx, int ly) {
        try {
            LinearLayout card = Ui.row(this);
            card.setGravity(Gravity.CENTER_VERTICAL);
            card.setBackground(Ui.strokedRound(this, 0xFF14171A, Ui.CARD_LINE, 10));
            card.setPadding(Ui.dp(this, 12), Ui.dp(this, 10), Ui.dp(this, 14), Ui.dp(this, 10));
            View sw = new View(this);
            LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(Ui.dp(this, 26), Ui.dp(this, 26));
            sl.rightMargin = Ui.dp(this, 10);
            sw.setBackground(Ui.strokedRound(this, 0xFF000000 | (rgb & 0xFFFFFF), Ui.CARD_LINE, 6));
            card.addView(sw, sl);
            card.addView(Ui.mono(this, String.format(java.util.Locale.US, "颜色 #%06X · 逻辑 (%d,%d)", rgb, lx, ly),
                    12, Ui.TEXT));
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams lp2 = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE,
                    PixelFormat.TRANSLUCENT);
            lp2.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
            lp2.y = Ui.dp(this, 110);
            wm.addView(card, lp2);
            h.postDelayed(new Runnable() {
                public void run() {
                    try { wm.removeView(card); } catch (Throwable ig) { }
                }
            }, 4000);
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "取色卡失败: " + t);
        }
    }

    private void setChipAlpha(float a) {
        if (chip == null || lp == null) return;
        try {
            lp.alpha = a;
            wm.updateViewLayout(chip, lp);
        } catch (Throwable ig) { }
    }

    /**
     * 回编辑器（v7.3 关键修复）：先把 SAW 可见窗（浮标）恢复可见并稍等——ColorOS/Android
     * 的后台启动豁免要求「调用方有可见悬浮窗」，否则 startActivity 会被静默丢弃；
     * 然后拉起 MainActivity（pick 坐标随 extra 带回），600ms 后收起浮标。
     */
    private void launchEditor(final int pickX, final int pickY, final int into) {
        if (chip == null) addChip();          /* 取点态可能还没建浮标：补一个当可见 SAW 窗 */
        if (chip != null && added) {
            setChipAlpha(1f);
            setText("\u2713", Ui.OK);
        }
        h.postDelayed(new Runnable() {
            public void run() {
                try {
                    Intent it = new Intent(CaptureChip.this, MainActivity.class);
                    it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    if (pickX >= 0 && pickY >= 0) {
                        it.putExtra("pick", 1);
                        it.putExtra("pickX", pickX);
                        it.putExtra("pickY", pickY);
                        it.putExtra("pickKind", pickKind);
                    } else if (into == 1) {
                        it.putExtra("into", "tmpl");
                    } else {
                        it.putExtra("visual", "latest");
                    }
                    startActivity(it);
                    TestBench.appendLog(CaptureChip.this, "chip_debug.txt",
                            "startActivity pick=" + (pickX >= 0) + " " + pickX + "," + pickY);
                } catch (Throwable t) {
                    TestBench.appendLog(CaptureChip.this, "chip_error.txt", "回跳失败: " + t);
                }
                h.postDelayed(new Runnable() {
                    public void run() { hideAll("launch-done"); }
                }, 600);
            }
        }, 150);
    }

    private void hideAll(String why) {
        running = false;
        TestBench.appendLog(this, "chip_debug.txt", "hideAll " + why);
        closeTestPanel();
        removePickOverlay();
        if (added && wm != null && chip != null) {
            try { wm.removeView(chip); } catch (Throwable ig) { }
        }
        added = false;
        try { stopForeground(true); } catch (Throwable ig) { }
        stopSelf();
    }

    @Override public void onDestroy() {
        running = false;
        TestBench.appendLog(this, "chip_debug.txt", "onDestroy");
        closeTestPanel();
        removePickOverlay();
        if (added && wm != null && chip != null) {
            try { wm.removeView(chip); } catch (Throwable ig) { }
        }
        added = false;
        h.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
