package com.vtouch.editor;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.widget.EditText;
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

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * 编辑悬浮窗（v8，浮标 + 面板）：
 *  · 武装态（编辑器 [去抓帧]）：单击=抓帧（自隐→raw→闪存直通→立即回编辑器，落盘后台继续）；长按=开面板。
 *  · 面板态（素材页 [悬浮试一下] 或长按）：单击=开/合面板。面板 = 编辑工具箱：
 *      [取图]=冻结帧框选建模板（v9.4，看图器组件：捏合缩放+角柄精调，全程悬浮窗不回 app） · [抓帧]=抓帧→帧库（留悬浮窗） · [取色]=全屏遮罩吞一次触摸 → 实时色值卡 · [框区域]=冻结帧框选建区（v12，矩形/圆，直写 regions.conf）
 *      · 测试找图/找色 = 选模板/点集 → 在当前屏幕实时找一次（不存帧，命中框标屏上）
 *      · 脚本测试 = 选操作 → 干跑（评估各视觉步命中，不执行触摸）→ 逐行报告 + 命中框标屏上。
 *  · 底栏 [打开主界面]（v9.2）= 收起面板（动画）并拉起编辑器主界面；浮标保留（可见窗豁免，口径同 launchEditor）。
 *  · v9.5：停用前台服务通知（原来是「vtouch 浮标」）——防冻靠可见浮标窗（floatWindow importance）
 *         + 系统白名单（bpm.xml/deviceidle）；拉起链路全走 startService / `am start-service`。
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

    /* v9.4 [取图]：冻结帧框选建模板浮层 */
    private FrameLayout tmplOverlay;
    private boolean tmplAdded = false;
    private ImageView tmplIv;                      /* 复用编辑部看图器（MODE_BOX）：捏合缩放/平移/角柄精调/像素网格 */
    private LinearLayout tmplCard, tmplBody;
    private EditText tmplNameE;
    private TextView tmplSaveB, tmplHintT;
    private boolean tmplOverwriteOk = false;
    private String tmplFreeName, tmplSaveDirOverride;
    private Capture.Shot tmplShot;
    private android.graphics.Bitmap tmplBmp;
    /* v12 [框区域]：冻结帧框选建区（矩形/圆）浮层 */
    private FrameLayout regionOverlay;
    private boolean regionAdded = false;
    private ImageView regionIv;
    private LinearLayout regionCard, regionBody;
    private TextView regionHintT, regionInfoT, regionSaveB, regionRectB, regionCircB;
    private Capture.Shot regionShot;
    private android.graphics.Bitmap regionBmp;
    private Regions.File regionFile;              /* 打开浮层时读一次（命名预览用）；保存时重读保证正确 */
    private int regionShape = 0;                  /* 0 矩形 1 圆（跨会话记忆：prefs） */
    private boolean regionSaving = false;         /* 防双击重复写 */
    private boolean regionHintErr = false;        /* 提示行处于错误态（画入或提交时解除） */
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
            c.startService(i);                                  /* v9.5：普通服务（不再 FGS，无通知） */
        } catch (Throwable t) { }
    }

    /** 素材页 [悬浮试一下]：拉起并直接展开测试面板（点素材 = 在当前屏幕实时找）。 */
    public static void startPanel(Context c) {
        Intent i = new Intent(c, CaptureChip.class);
        i.putExtra("panel", true);
        try {
            c.startService(i);                                  /* v9.5：普通服务（不再 FGS，无通知） */
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
            c.startService(i);                                  /* v9.5：普通服务（不再 FGS，无通知） */
        } catch (Throwable t) { }
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        /* v9.5：不再前台服务化（去掉无法隐藏的「vtouch 浮标」通知）——防 ColorOS 速冻靠**可见浮标窗**
         * （importance 抬到 floatWindow；前台服务本来也拦不住冻结，见 ChipService 注释）+ 系统白名单。
         * 拉起改走 `am start-service` / startService（本应用全部拉起路径都在前台，不受后台启动限制）。 */
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
        } else if ("main".equals(demo)) {                    /* v9.2 调试：[打开主界面] 全链（开面板 → 1.5s 触发） */
            if (!panelAdded) openTestPanel();
            h.postDelayed(new Runnable() {
                public void run() { openMainApp(); }
            }, 1500);
        } else if ("tmpl".equals(demo)) {                    /* v9.4 调试：[取图] 冻结框选全链（画框→取名聚焦→scratch 自动保存） */
            if (!panelAdded) openTestPanel();
            h.postDelayed(new Runnable() {
                public void run() { startTmplPick(); }
            }, 1200);
            h.postDelayed(new Runnable() {
                public void run() { tmplDemoBox(); }
            }, 3200);
            h.postDelayed(new Runnable() {
                public void run() { tmplDemoSave(); }
            }, 5600);
            h.postDelayed(new Runnable() {
                public void run() { if (tmplAdded) tmplCancel(); }      /* 兜底：异常时不留残留浮层 */
            }, 9000);
        } else if ("region".equals(demo)) {                  /* v12 调试：[框区域] 冻结框选全链（两轮：矩形→圆；自动建区到 scratch；不碰真实 regions.conf） */
            if (!panelAdded) openTestPanel();
            h.postDelayed(new Runnable() {
                public void run() { startRegionPick(); }
            }, 1200);
            h.postDelayed(new Runnable() {
                public void run() { regionDemoBox(); }
            }, 3200);
            h.postDelayed(new Runnable() {
                public void run() { regionDemoSave(); }
            }, 5600);
            h.postDelayed(new Runnable() {
                public void run() { regionDemoStartCircle(); }      /* 第二轮：圆 */
            }, 7500);
            h.postDelayed(new Runnable() {
                public void run() { regionDemoBoxCirc(); }
            }, 9500);
            h.postDelayed(new Runnable() {
                public void run() { regionDemoSave(); }
            }, 11500);
            h.postDelayed(new Runnable() {
                public void run() {                                  /* 兜底：复位形状记忆，异常时不留残留浮层 */
                    try {
                        getSharedPreferences("chip", MODE_PRIVATE).edit().putInt("regionShape", 0).apply();
                    } catch (Throwable ig) { }
                    if (regionAdded) regionCancel();
                }
            }, 15000);
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
        } else if ("ping".equals(demo)) {                    /* v9.5 调试：存活探针（只写日志、不动 UI；冻结判定用） */
            TestBench.appendLog(this, "chip_debug.txt", "ping");
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

    /** 抓帧（武装链/调试：抓完回编辑器看新帧）。 */
    private void doCapture() { doCapture(0, true); }

    /** 抓帧：自隐 → 抓 → 闪存直通；落盘后台继续。
     *  into=0 帧库、into=2 进 [取图] 冻结框选（v9.4）、into=3 进 [框区域]（v12）；back=true 抓完回编辑器（武装链），false 留在悬浮窗（面板）。 */
    private void doCapture(final int into, final boolean back) {
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
                                if (into == 2) {                     /* v9.4 取图：冻结帧框选（留在悬浮窗） */
                                    onTmplCaptured(fshot);
                                    return;
                                }
                                if (into == 3) {                     /* v12 框区域：冻结帧框选建区（留在悬浮窗） */
                                    onRegionCaptured(fshot);
                                    return;
                                }
                                if (back) {
                                    launchEditor(-1, -1, into);      /* 武装链：抓完回编辑器 */
                                    return;
                                }
                                /* v9.4 面板 [抓帧]：留在悬浮窗（帧已入帧库） */
                                h.postDelayed(paintBack, 1500);
                                syncChipVisibility();
                                if (panelAdded && testPanel != null) testPanel.setVisibility(View.VISIBLE);
                                if (panelResult != null) {
                                    panelResult.setText("\u2713 已抓帧 " + fshot.w + "\u00d7" + fshot.h
                                            + " · 已入帧库（[打开主界面] 可查看）");
                                    panelResult.setTextColor(Ui.OK);
                                }
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
            TextView x = Ui.btnFlat(this, "✕", Ui.DIM);
            x.setTextSize(15);
            x.setMinWidth(Ui.dp(this, 48));
            x.setMinHeight(Ui.dp(this, 48));
            forgivingTap(x, new Runnable() {                 /* v9.2：头部是拖动区，标准 click 位移 >8dp 静默取消 = 不好点 → 宽容口径 */
                public void run() { closeTestPanelAnimated(); }
            });
            head.addView(x);
            col.addView(head);
            panelResult = Ui.mono(this, "取图→模板 · 抓帧→帧库 · 取色→色值 · 框区域→建区", 11, Ui.DIM);
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
            TextView openMain = Ui.btnFlat(this, "打开主界面", Ui.ACCENT);   /* v9.2：悬浮窗 → 编辑器主界面（底栏常驻，任何子视图可达） */
            openMain.setMinHeight(Ui.dp(this, 44));
            openMain.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { openMainApp(); }
            });
            LinearLayout.LayoutParams olp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            olp.rightMargin = Ui.dp(this, 8);
            bts.addView(openMain, olp);
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

    /** v9.2 面板 [打开主界面]：收起面板（动画，浮标随后复现）→ 等落定再拉起编辑器 MainActivity；
     *  浮标保留在屏（可见窗+可见浮标 = 后台启动豁免；口径同 launchEditor），不退出悬浮窗。 */
    private void openMainApp() {
        if (capturing || closingPanel != null) return;
        TestBench.appendLog(this, "chip_debug.txt", "openMainApp");
        if (chip == null) addChip();
        if (chip != null && added) setChipAlpha(1f);         /* 窗口层保持「可见」（SAW/启动豁免） */
        if (panelAdded) closeTestPanelAnimated();
        h.postDelayed(new Runnable() {                       /* 等收起动画落定 + 浮标淡入可见再跳 */
            public void run() {
                try {
                    Intent it = new Intent(CaptureChip.this, MainActivity.class);
                    it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(it);
                    TestBench.appendLog(CaptureChip.this, "chip_debug.txt", "openMainApp started");
                } catch (Throwable t) {
                    TestBench.appendLog(CaptureChip.this, "chip_error.txt", "打开主界面失败: " + t);
                }
            }
        }, 260);
    }

    /** 宽容点按（v9.2）：头部是拖动区，标准 click 在位移 >8dp 时静默取消（拖拽又不接手）=「不太好点」；
     *  此口径：按下即反馈，松手位移 ≤16dp 即触发，超出视为非点按（不触发、也不拖动窗口）。 */
    private void forgivingTap(final View v, final Runnable onTap) {
        final float max = Ui.dp(this, 16);
        v.setOnTouchListener(new View.OnTouchListener() {
            float dx, dy;
            boolean dead;
            public boolean onTouch(View w, MotionEvent e) {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        dx = e.getX();
                        dy = e.getY();
                        dead = false;
                        w.setPressed(true);
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        if (!dead && Math.abs(e.getX() - dx) + Math.abs(e.getY() - dy) > max) {
                            dead = true;
                            w.setPressed(false);
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                        w.setPressed(false);
                        if (!dead) onTap.run();
                        return true;
                    case MotionEvent.ACTION_CANCEL:
                        w.setPressed(false);
                        return true;
                }
                return false;
            }
        });
    }

    /** [取图]（v9.4，用户口径）：截当前屏 → 冻结帧上框选 → 直接存模板；全程悬浮窗，不回编辑器。 */
    private void startTmplPick() {
        if (capturing || closingPanel != null) return;
        TestBench.appendLog(this, "chip_debug.txt", "startTmplPick");
        doCapture(2, false);
    }

    /** 取图抓帧成功（主线程）：收起面板（此刻已隐，直接收掉不走动画）→ 建冻结帧框选浮层。 */
    private void onTmplCaptured(final Capture.Shot s) {
        tmplShot = s;
        TestBench.appendLog(this, "chip_debug.txt", "tmplCaptured " + s.w + "x" + s.h);
        if (panelAdded || closingPanel != null) closeTestPanel();
        Ui.bg(new Runnable() {
            public void run() {
                final android.graphics.Bitmap bmp = FrameStore.bitmapFromRgba(s.rgba, s.w, s.h);
                final String free = freeTmplName();
                h.post(new Runnable() {
                    public void run() {
                        tmplBmp = bmp;
                        tmplFreeName = free;
                        addTmplOverlay();
                    }
                });
            }
        }, null);
    }

    /** 建冻结帧框选浮层：底=冻结帧图 + 框选绘制/触摸层 + 顶部取名卡（可聚焦窗，取名走系统 IME）。 */
    private void addTmplOverlay() {
        try {
            if (tmplAdded) return;
            if (wm == null) wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            FrameLayout root = new FrameLayout(this);
            tmplIv = new ImageView(this);                        /* 看图器：拖动新框 / 框内挪框 / 角柄精调 / 双指缩放平移 */
            tmplIv.setMode(ImageView.MODE_BOX);
            if (tmplBmp != null) tmplIv.setFrame(tmplBmp, tmplShot != null ? tmplShot.rot : 0);
            tmplIv.setListener(new ImageView.Listener() {
                public void onBox(int x, int y, int w, int h) {
                    if (w >= 8 && h >= 8) setTmplEditing(true);
                }
                public void onPoint(int x, int y) { }
            });
            root.addView(tmplIv, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            root.addView(buildTmplCard(), tmplCardLp());
            root.setFocusableInTouchMode(true);                  /* 聚焦窗：BACK 取消 + 命名走 IME */
            root.requestFocus();
            root.setOnKeyListener(new View.OnKeyListener() {
                public boolean onKey(View v, int code, android.view.KeyEvent e) {
                    if (code == android.view.KeyEvent.KEYCODE_BACK
                            && e.getAction() == android.view.KeyEvent.ACTION_UP) {
                        tmplCancel();
                        return true;
                    }
                    return false;
                }
            });
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams plp = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.MATCH_PARENT,
                    type,
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,        /* 不加 NOT_FOCUSABLE：取名输入需要 IME */
                    PixelFormat.TRANSLUCENT);
            plp.gravity = Gravity.TOP | Gravity.LEFT;
            plp.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING;   /* 全屏窗不随键盘重排 */
            if (Build.VERSION.SDK_INT >= 30) {
                try { plp.setFitInsetsTypes(0); }             /* v9.4.1 修：原被系统 inset 压成 1440×3006（图纵压~5%+坐标偏） */
                catch (Throwable ig) { }
            }
            wm.addView(root, plp);
            tmplOverlay = root;
            tmplAdded = true;
            TestBench.appendLog(this, "chip_debug.txt", "tmplOverlay added");
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "取图浮层添加失败: " + t);
        }
    }

    private FrameLayout.LayoutParams tmplCardLp() {
        FrameLayout.LayoutParams clp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        clp.topMargin = Ui.dp(this, 52);
        clp.leftMargin = Ui.dp(this, 10);
        clp.rightMargin = Ui.dp(this, 10);
        return clp;
    }

    /** 顶部卡：提示行（常显）+ 取名/保存区（框选完成后显示）。 */
    private LinearLayout buildTmplCard() {
        tmplCard = Ui.col(this);
        tmplCard.setBackground(Ui.strokedRound(this, 0xF214171A, Ui.CARD_LINE, 12));
        tmplCard.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 8));
        LinearLayout r1 = Ui.row(this);
        r1.setGravity(Gravity.CENTER_VERTICAL);
        tmplHintT = Ui.tv(this, "取图 · 拖框选区域（双指缩放/角柄精调）", 13, Ui.TEXT, true);
        tmplHintT.setSingleLine(true);
        tmplHintT.setEllipsize(TextUtils.TruncateAt.END);
        r1.addView(tmplHintT, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView cancel = Ui.btnFlat(this, "\u2715 取消", Ui.DIM);
        cancel.setMinHeight(Ui.dp(this, 44));
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { tmplCancel(); }
        });
        r1.addView(cancel);
        tmplCard.addView(r1);
        tmplBody = Ui.col(this);
        tmplBody.setVisibility(View.GONE);
        LinearLayout r2 = Ui.row(this);
        r2.setGravity(Gravity.CENTER_VERTICAL);
        r2.setPadding(0, Ui.dp(this, 2), 0, 0);
        TextView nl = Ui.tv(this, "名称", 12, Ui.DIM, false);
        nl.setPadding(Ui.dp(this, 2), 0, Ui.dp(this, 8), 0);
        r2.addView(nl);
        tmplNameE = new EditText(this);
        tmplNameE.setHint("如 tmpl1");
        tmplNameE.setHintTextColor(Ui.FAINT);
        tmplNameE.setTextColor(Ui.TEXT);
        tmplNameE.setTextSize(14);
        tmplNameE.setTypeface(Ui.monoFont(this));
        tmplNameE.setSingleLine(true);
        tmplNameE.setFilters(new android.text.InputFilter[]{ new android.text.InputFilter.LengthFilter(15) });
        tmplNameE.setBackground(Ui.strokedRound(this, Ui.CARD2, Ui.CARD_LINE, 8));
        tmplNameE.setPadding(Ui.dp(this, 10), Ui.dp(this, 8), Ui.dp(this, 10), Ui.dp(this, 8));
        tmplNameE.setText(tmplFreeName != null ? tmplFreeName : "tmpl1");
        tmplNameE.addTextChangedListener(new android.text.TextWatcher() {
            public void beforeTextChanged(CharSequence s2, int st, int cnt, int af) { }
            public void onTextChanged(CharSequence s2, int st, int b2, int cnt) { }
            public void afterTextChanged(android.text.Editable s2) {
                tmplOverwriteOk = false;
                if (tmplSaveB != null) tmplSaveB.setText("保存模板");
                if (tmplHintT != null) {
                    tmplHintT.setText("取图 · 框选完成，取名保存");
                    tmplHintT.setTextColor(Ui.TEXT);
                }
            }
        });
        r2.addView(tmplNameE, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView rebox = Ui.btnFlat(this, "重框", Ui.DIM);
        rebox.setMinHeight(Ui.dp(this, 44));
        rebox.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (tmplIv != null) tmplIv.clearSelection();
                setTmplEditing(false);
            }
        });
        r2.addView(rebox);
        tmplBody.addView(r2);
        tmplSaveB = Ui.btnSmallPrimary(this, "保存模板");
        tmplSaveB.setMinHeight(Ui.dp(this, 44));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        slp.topMargin = Ui.dp(this, 4);
        tmplSaveB.setLayoutParams(slp);
        tmplSaveB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { saveTmplFromSel(); }
        });
        tmplBody.addView(tmplSaveB);
        tmplCard.addView(tmplBody);
        return tmplCard;
    }

    /** 切换「框选中 / 框选完成」两态（提示行文案 + 取名区显隐 + 覆盖确认复位）。 */
    private void setTmplEditing(boolean editing) {
        if (tmplBody != null) tmplBody.setVisibility(editing ? View.VISIBLE : View.GONE);
        if (tmplHintT != null) {
            tmplHintT.setText(editing ? "取图 · 框选完成，取名保存" : "取图 · 拖框选区域（双指缩放/角柄精调）");
            tmplHintT.setTextColor(Ui.TEXT);
        }
        if (!editing) {
            tmplOverwriteOk = false;
            if (tmplSaveB != null) tmplSaveB.setText("保存模板");
        }
    }

    /** [保存模板]：裁剪冻结帧 → 灰度/均色（与面板写端同算式）→ 原子写 .tmpl；完成后收浮层回面板。 */
    private void saveTmplFromSel() {
        if (tmplShot == null || tmplIv == null || !tmplAdded) return;
        final String name = tmplNameE != null ? tmplNameE.getText().toString().trim() : "";
        if (!Assets.nameOk(name)) {
            if (tmplHintT != null) { tmplHintT.setText("名称不合法（1..15 个可见字符，可用中文）"); tmplHintT.setTextColor(Ui.WARN); }
            return;
        }
        if (Assets.exists(name) && !tmplOverwriteOk) {          /* 同名：首点变「确认覆盖」两步确认 */
            tmplOverwriteOk = true;
            if (tmplSaveB != null) tmplSaveB.setText("确认覆盖");
            if (tmplHintT != null) {
                tmplHintT.setText("已存在同名素材「" + name + "」，再点一次覆盖");
                tmplHintT.setTextColor(Ui.WARN);
            }
            return;
        }
        int[] sb = tmplIv.selection();
        if (sb[2] < 8 || sb[3] < 8) {
            if (tmplHintT != null) {
                tmplHintT.setText("先在画面上框选区域（拖动即画框）");
                tmplHintT.setTextColor(Ui.WARN);
            }
            return;
        }
        int x = Math.max(0, sb[0]);
        int y = Math.max(0, sb[1]);
        int bw = Math.min(tmplShot.w - x, sb[2]);
        int bh = Math.min(tmplShot.h - y, sb[3]);
        if (bw < 1 || bh < 1) return;
        final int fx = x, fy = y, fw = bw, fh = bh;
        final Capture.Shot s = tmplShot;
        if (tmplHintT != null) { tmplHintT.setText("保存中…"); tmplHintT.setTextColor(Ui.DIM); }
        Ui.bg(new Runnable() {
            public void run() {
                String err = null;
                String realDir = Assets.dir;
                if (tmplSaveDirOverride != null) Assets.dir = tmplSaveDirOverride;   /* 仅 demo 调试用 */
                try {
                    int[] avg = new int[3];
                    byte[] gray = Assets.grayFromRgba(s.rgba, s.w, fx, fy, fw, fh, avg);
                    if (gray == null) err = "框选区超出帧";
                    else {
                        Assets.Tmpl t = new Assets.Tmpl();
                        t.w = fw;
                        t.h = fh;
                        t.rot = s.rot;
                        t.x = fx;
                        t.y = fy;
                        t.hasXy = true;
                        t.avgRgb = (avg[0] << 16) | (avg[1] << 8) | avg[2];
                        t.gray = gray;
                        Assets.writeTmpl(name, t);
                    }
                } catch (Throwable tt) {
                    err = String.valueOf(tt);
                } finally {
                    Assets.dir = realDir;
                }
                final String fe = err;
                h.post(new Runnable() {
                    public void run() {
                        tmplSaveDirOverride = null;
                        if (fe != null) {
                            TestBench.appendLog(CaptureChip.this, "chip_error.txt", "存模板失败: " + fe);
                            if (tmplHintT != null) { tmplHintT.setText("保存失败：" + fe); tmplHintT.setTextColor(Ui.WARN); }
                            return;
                        }
                        TestBench.appendLog(CaptureChip.this, "chip_debug.txt",
                                "tmplSaved " + name + " " + fx + "," + fy + " " + fw + "x" + fh);
                        removeTmplOverlay();
                        showResultToast("\u2713 已存模板 " + name, true);
                        if (!panelAdded) openTestPanel();           /* 回面板（悬浮窗留驻） */
                    }
                });
            }
        }, null);
    }

    /** 首空模板名（与编辑部同名口径：tmpl1/tmpl2…；模板/点集共用名字空间）。 */
    private String freeTmplName() {
        List<String> all = new ArrayList<String>();
        all.addAll(Assets.namesTmpl());
        all.addAll(Assets.namesPts());
        for (int i = 1; i < 1000; i++) {
            String cand = "tmpl" + i;
            if (!all.contains(cand)) return cand;
        }
        return "tmpl";
    }

    /** 取消取图：收浮层 → 回面板（悬浮窗留驻）。 */
    private void tmplCancel() {
        if (!tmplAdded) return;
        TestBench.appendLog(this, "chip_debug.txt", "tmplCancel");
        removeTmplOverlay();
        if (!panelAdded) openTestPanel();
    }

    /** 收起取图浮层（保存完成 / 取消 / 服务收尾共用）。 */
    private void removeTmplOverlay() {
        if (tmplAdded && tmplOverlay != null && wm != null) {
            try { wm.removeView(tmplOverlay); } catch (Throwable ig) { }
        }
        tmplOverlay = null;
        tmplAdded = false;
        tmplIv = null;
        tmplCard = null;
        tmplBody = null;
        tmplNameE = null;
        tmplSaveB = null;
        tmplHintT = null;
        tmplShot = null;
        tmplBmp = null;
        tmplSaveDirOverride = null;
        tmplOverwriteOk = false;
    }

    /** v9.4 调试（demo tmpl）：自动画框 + 聚焦取名（验 IME）。 */
    private void tmplDemoBox() {
        if (!tmplAdded || tmplIv == null || tmplShot == null) return;
        int w = tmplShot.w, h = tmplShot.h;
        tmplIv.setSelection(Math.round(w * 0.25f), Math.round(h * 0.30f),
                Math.max(8, Math.round(w * 0.5f)), Math.max(8, Math.round(h * 0.32f)));
        setTmplEditing(true);
        if (tmplNameE != null) {
            tmplNameE.requestFocus();
            try {
                android.view.inputmethod.InputMethodManager imm =
                        (android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                imm.showSoftInput(tmplNameE, 0);
            } catch (Throwable ig) { }
        }
        int[] db = tmplIv.selection();
        TestBench.appendLog(this, "chip_debug.txt", "tmplDemoBox " + db[0] + "," + db[1] + " " + db[2] + "x" + db[3]
                + " | overlay=" + (tmplOverlay != null ? tmplOverlay.getWidth() + "x" + tmplOverlay.getHeight() : "?"));
    }

    /** v9.4 调试（demo tmpl）：自动保存（重定向 scratch 目录，不碰真实模板库）。 */
    private void tmplDemoSave() {
        if (!tmplAdded) return;
        tmplSaveDirOverride = Ops.TMPL_DIR + "-demo";
        saveTmplFromSel();
    }

    /* ---- v12 [框区域]：冻结帧框选建区（矩形/圆）→ 直写 regions.conf（备份 + 原子写 + 镜像）----
     * 交互口径（能力与面板一致、能少一步就少一步）：
     *   面板 [框区域] → 冻结当前屏 → 拖框（或按下定圆心、拖动定半径）→ [建区]（名字自动 r1/c1…）。
     *   形状 [矩形]/[圆] 在浮层里随时切（跨会话记忆）；[重框] 清掉重画；✕ / BACK 取消。
     *   坐标链 = 帧像素（看图器）→ 竖屏逻辑（Matcher.frameToLogic，区域表口径）。
     *   demo region：自动画框 → 自动建区到 scratch（RegionDraw.pathOverride），不碰真实 regions.conf。 */

    /** [框区域]：读形状记忆 → 截当前屏 → 冻结帧框选建区。 */
    private void startRegionPick() {
        if (capturing || closingPanel != null || regionAdded) return;
        regionShape = getSharedPreferences("chip", MODE_PRIVATE).getInt("regionShape", 0);
        TestBench.appendLog(this, "chip_debug.txt", "startRegionPick shape=" + regionShape);
        doCapture(3, false);
    }

    /** 抓帧成功（主线程）：收起面板 → 读区域文件（命名预览）→ 建冻结帧建区浮层。 */
    private void onRegionCaptured(final Capture.Shot s) {
        regionShot = s;
        TestBench.appendLog(this, "chip_debug.txt", "regionCaptured " + s.w + "x" + s.h);
        if (panelAdded || closingPanel != null) closeTestPanel();
        Ui.bg(new Runnable() {
            public void run() {
                final android.graphics.Bitmap bmp = FrameStore.bitmapFromRgba(s.rgba, s.w, s.h);
                Regions.File rf = null;
                String err = null;
                try {
                    rf = RegionDraw.loadLive();
                } catch (Throwable t) {
                    err = String.valueOf(t.getMessage() != null ? t.getMessage() : t);
                }
                final Regions.File frf = rf;
                final String fe = err;
                h.post(new Runnable() {
                    public void run() {
                        if (fe != null) {
                            regionShot = null;
                            showResultToast("读取 regions.conf 失败：" + fe, false);
                            if (!panelAdded) openTestPanel();
                            return;
                        }
                        regionFile = frf;
                        regionBmp = bmp;
                        addRegionOverlay();
                    }
                });
            }
        }, null);
    }

    /** 建建区浮层：冻结帧图 + 框选/圆绘制层 + 顶部卡（形状 / [重框] / [建区]）+ BACK 取消。 */
    private void addRegionOverlay() {
        try {
            if (regionAdded) return;
            if (wm == null) wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            FrameLayout root = new FrameLayout(this);
            regionIv = new ImageView(this);
            regionIv.setMode(regionShape == 1 ? ImageView.MODE_CIRCLE : ImageView.MODE_BOX);
            if (regionBmp != null) regionIv.setFrame(regionBmp, regionShot != null ? regionShot.rot : 0);
            regionIv.setListener(new ImageView.Listener() {
                public void onBox(int x, int y, int w, int h) {
                    boolean ok = w >= 8 && h >= 8;
                    setRegionEditing(ok);
                    if (ok) updateRegionInfo();
                }
                public void onPoint(int x, int y) { }
            });
            regionIv.setCircleListener(new ImageView.CircleListener() {
                public void onCircle(int cx, int cy, int r) {
                    boolean ok = r >= 8;
                    setRegionEditing(ok);
                    if (ok) updateRegionInfo();
                }
            });
            root.addView(regionIv, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            root.addView(buildRegionCard(), tmplCardLp());
            root.setFocusableInTouchMode(true);                  /* 聚焦窗：BACK 取消 */
            root.requestFocus();
            root.setOnKeyListener(new View.OnKeyListener() {
                public boolean onKey(View v, int code, android.view.KeyEvent e) {
                    if (code == android.view.KeyEvent.KEYCODE_BACK
                            && e.getAction() == android.view.KeyEvent.ACTION_UP) {
                        regionCancel();
                        return true;
                    }
                    return false;
                }
            });
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams plp = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.MATCH_PARENT,
                    type,
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                    PixelFormat.TRANSLUCENT);
            plp.gravity = Gravity.TOP | Gravity.LEFT;
            if (Build.VERSION.SDK_INT >= 30) {
                try { plp.setFitInsetsTypes(0); } catch (Throwable ig) { }
            }
            wm.addView(root, plp);
            regionOverlay = root;
            regionAdded = true;
            TestBench.appendLog(this, "chip_debug.txt", "regionOverlay added");
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "框区域浮层添加失败: " + t);
        }
    }

    /** 顶部卡：提示行（常显）+ 形状 / [重框] / [建区] 区（画完后显示）。 */
    private LinearLayout buildRegionCard() {
        regionCard = Ui.col(this);
        regionCard.setBackground(Ui.strokedRound(this, 0xF214171A, Ui.CARD_LINE, 12));
        regionCard.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 8));
        LinearLayout r1 = Ui.row(this);
        r1.setGravity(Gravity.CENTER_VERTICAL);
        regionHintT = Ui.tv(this, regionShape == 1 ? "框区域 · 按下定圆心，拖动定半径"
                                                   : "框区域 · 拖框选区域（双指缩放/角柄精调）", 13, Ui.TEXT, true);
        regionHintT.setSingleLine(true);
        regionHintT.setEllipsize(TextUtils.TruncateAt.END);
        r1.addView(regionHintT, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView cancel = Ui.btnFlat(this, "\u2715 取消", Ui.DIM);
        cancel.setMinHeight(Ui.dp(this, 44));
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { regionCancel(); }
        });
        r1.addView(cancel);
        regionCard.addView(r1);
        regionBody = Ui.col(this);
        regionBody.setVisibility(View.GONE);
        LinearLayout r2 = Ui.row(this);
        r2.setGravity(Gravity.CENTER_VERTICAL);
        r2.setPadding(0, Ui.dp(this, 2), 0, 0);
        regionRectB = Ui.btnFlat(this, "矩形", Ui.ACCENT);
        regionRectB.setMinHeight(Ui.dp(this, 44));
        regionRectB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { setRegionShape(0); }
        });
        r2.addView(regionRectB);
        regionCircB = Ui.btnFlat(this, "圆", Ui.DIM);
        regionCircB.setMinHeight(Ui.dp(this, 44));
        regionCircB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { setRegionShape(1); }
        });
        r2.addView(regionCircB);
        r2.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1f));
        TextView rebox = Ui.btnFlat(this, "重框", Ui.DIM);
        rebox.setMinHeight(Ui.dp(this, 44));
        rebox.setOnClickListener(new View.OnClickListener() {
        public void onClick(View v) {
            regionHintErr = false;
            if (regionIv != null) { regionIv.clearSelection(); regionIv.clearCircle(); }
            setRegionEditing(false);
        }
        });
        r2.addView(rebox);
        regionBody.addView(r2);
        regionInfoT = Ui.mono(this, "", 11, Ui.DIM);
        regionInfoT.setSingleLine(false);
        regionInfoT.setPadding(Ui.dp(this, 2), Ui.dp(this, 2), 0, Ui.dp(this, 2));
        regionBody.addView(regionInfoT);
        regionSaveB = Ui.btnSmallPrimary(this, "建区");
        regionSaveB.setMinHeight(Ui.dp(this, 44));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        slp.topMargin = Ui.dp(this, 4);
        regionSaveB.setLayoutParams(slp);
        regionSaveB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { saveRegionFromSel(); }
        });
        regionBody.addView(regionSaveB);
        regionCard.addView(regionBody);
        updateRegionShapeBtns();
        return regionCard;
    }

    /** 形状切换：[矩形]/[圆]（清掉旧画、记忆到 prefs、切看图器模式）。 */
    private void setRegionShape(int s) {
        regionHintErr = false;
        if (regionShape == s) return;
        regionShape = s;
        try {
            getSharedPreferences("chip", MODE_PRIVATE).edit().putInt("regionShape", s).apply();
        } catch (Throwable ig) { }
        if (regionIv != null) {
            regionIv.clearSelection();
            regionIv.clearCircle();
            regionIv.setMode(s == 1 ? ImageView.MODE_CIRCLE : ImageView.MODE_BOX);
        }
        updateRegionShapeBtns();
        setRegionEditing(false);
        if (regionHintT != null)
            regionHintT.setText(s == 1 ? "框区域 · 按下定圆心，拖动定半径"
                                       : "框区域 · 拖框选区域（双指缩放/角柄精调）");
    }

    private void updateRegionShapeBtns() {
        if (regionRectB != null) regionRectB.setTextColor(regionShape == 0 ? Ui.ACCENT : Ui.DIM);
        if (regionCircB != null) regionCircB.setTextColor(regionShape == 1 ? Ui.ACCENT : Ui.DIM);
    }

    /** 切换「画中 / 画完」两态（提示行 + 底部区显隐）。 */
    private void setRegionEditing(boolean editing) {
        if (regionBody != null) regionBody.setVisibility(editing ? View.VISIBLE : View.GONE);
        if (regionHintT == null) return;
        if (editing) regionHintErr = false;          /* 重新画出内容：解除错误态 */
        else if (regionHintErr) return;              /* 保持错误提示，直到下次画入 / 切换形状 / 重框 */
        if (editing)
            regionHintT.setText(regionShape == 1 ? "框区域 · 圆已就位，可 [建区] 或 [重框]"
                                                 : "框区域 · 框已画好，可 [建区] 或 [重框]");
        else
            regionHintT.setText(regionShape == 1 ? "框区域 · 按下定圆心，拖动定半径"
                                                 : "框区域 · 拖框选区域（双指缩放/角柄精调）");
    }

    /** 预览行：将建的名字 + 逻辑坐标（名字取「首个空闲」；保存时会重读重算保证正确）。 */
    private void updateRegionInfo() {
        if (regionInfoT == null || regionIv == null || regionShot == null) return;
        /* v12e：区域数字口径 = 基帧（#frame）——预览与保存同源，不随捕获帧方向转。 */
        int rot = regionFile != null ? regionFile.frameRot : 0;
        int fw = regionFile != null ? regionFile.frameW : 0;
        int fh = regionFile != null ? regionFile.frameH : 0;
        if (regionShape == 1) {
            int[] c = regionIv.circle();
            if (c[2] < 8) return;
            int[] L = RegionDraw.circleToLogic(rot, fw, fh, c[0], c[1], c[2]);
            regionInfoT.setText("将建 " + RegionDraw.nextId(regionFile, true)
                    + " · 圆  圆心 " + L[0] + "," + L[1] + "  r=" + L[2]);
        } else {
            int[] b = regionIv.selection();
            if (b[2] < 8 || b[3] < 8) return;
            int[] L = RegionDraw.rectToLogic(rot, fw, fh, b[0], b[1], b[2], b[3]);
            regionInfoT.setText("将建 " + RegionDraw.nextId(regionFile, false)
                    + " · 矩形  " + L[0] + "," + L[1] + " → " + L[2] + "," + L[3]);
        }
    }

    /** [建区]：换算逻辑坐标 → 重读 live 文件 → 定名 → 校验 → 备份 + 原子写 + 镜像；完事收浮层回面板。 */
    private void saveRegionFromSel() {
        if (regionSaving || regionShot == null || regionIv == null || !regionAdded) return;
        final boolean circle = regionShape == 1;
        final int[] g;                       /* 帧上几何（v12e：换算在 bg 里按刚读的盘上基帧做） */
        if (circle) {
            int[] c = regionIv.circle();
            if (c[2] < 8) { regionHintWarn("先在画面上画一个圆（按下定圆心，拖动定半径）"); return; }
            g = new int[]{c[0], c[1], c[2], 0};
        } else {
            int[] b = regionIv.selection();
            if (b[2] < 8 || b[3] < 8) { regionHintWarn("先在画面上框选区域（拖动即画框）"); return; }
            g = new int[]{b[0], b[1], b[2], b[3]};
        }
        regionSaving = true;
        regionHintErr = false;
        if (regionHintT != null) { regionHintT.setText("写入中…"); regionHintT.setTextColor(Ui.DIM); }
        Ui.bg(new Runnable() {
            public void run() {
                String err = null, id = null;
                final int[] vals = new int[4];
                try {
                    Regions.File f = RegionDraw.loadLive();
                    int[] L = circle
                            ? RegionDraw.circleToLogic(f.frameRot, f.frameW, f.frameH, g[0], g[1], g[2])
                            : RegionDraw.rectToLogic(f.frameRot, f.frameW, f.frameH, g[0], g[1], g[2], g[3]);
                    vals[0] = L[0]; vals[1] = L[1]; vals[2] = L[2]; vals[3] = L[3];
                    id = RegionDraw.nextId(f, circle);
                    RegionDraw.save(f, RegionDraw.make(id, circle, vals[0], vals[1], vals[2], circle ? 0 : vals[3]));
                } catch (Throwable t) {
                    err = String.valueOf(t.getMessage() != null ? t.getMessage() : t);
                }
                final String fe = err, fid = id;
                h.post(new Runnable() {
                    public void run() {
                        regionSaving = false;
                        RegionDraw.pathOverride = null;
                        if (fe != null) {
                            TestBench.appendLog(CaptureChip.this, "chip_error.txt", "建区失败: " + fe);
                            regionHintWarn("建区失败：" + fe);
                            return;
                        }
                        boolean running = !Su.corePid().isEmpty();
                        String geom = circle ? ("圆 圆心 " + vals[0] + "," + vals[1] + " r=" + vals[2])
                                             : ("矩形 " + vals[0] + "," + vals[1] + " → " + vals[2] + "," + vals[3]);
                        TestBench.appendLog(CaptureChip.this, "chip_debug.txt",
                                "regionSaved " + fid + " " + (circle ? "circ" : "rect") + " " + geom
                                + " | core=" + (running ? "runs" : "stopped"));
                        removeRegionOverlay();
                        showResultToast("\u2713 已建区域 " + fid + "（" + (circle ? "圆" : "矩形") + "）"
                                + (running ? " · 约 1–2 秒生效" : " · 核心启动时生效"), true);
                        if (!panelAdded) openTestPanel();
                    }
                });
            }
        }, null);
    }

    private void regionHintWarn(String s) {
        regionHintErr = true;
        if (regionHintT != null) { regionHintT.setText(s); regionHintT.setTextColor(Ui.WARN); }
    }

    /** 取消框区域：收浮层 → 回面板（悬浮窗留驻）。 */
    private void regionCancel() {
        if (!regionAdded) return;
        TestBench.appendLog(this, "chip_debug.txt", "regionCancel");
        removeRegionOverlay();
        if (!panelAdded) openTestPanel();
    }

    /** 收起建区浮层（保存完成 / 取消 / 服务收尾共用）。 */
    private void removeRegionOverlay() {
        if (regionAdded && regionOverlay != null && wm != null) {
            try { wm.removeView(regionOverlay); } catch (Throwable ig) { }
        }
        regionOverlay = null;
        regionAdded = false;
        regionIv = null;
        regionCard = null;
        regionBody = null;
        regionRectB = null;
        regionCircB = null;
        regionSaveB = null;
        regionInfoT = null;
        regionHintT = null;
        regionShot = null;
        regionBmp = null;
        regionFile = null;
        regionSaving = false;
        RegionDraw.pathOverride = null;
    }

    /** v12 调试（demo region）：切回矩形并自动画框 + 进编辑态。 */
    private void regionDemoBox() {
        if (!regionAdded || regionIv == null || regionShot == null) return;
        if (regionShape != 0) setRegionShape(0);
        int w = regionShot.w, h = regionShot.h;
        regionIv.setSelection(Math.round(w * 0.25f), Math.round(h * 0.30f),
                Math.max(8, Math.round(w * 0.5f)), Math.max(8, Math.round(h * 0.32f)));
        setRegionEditing(true);
        updateRegionInfo();
        int[] db = regionIv.selection();
        TestBench.appendLog(this, "chip_debug.txt", "regionDemoBox " + db[0] + "," + db[1] + " " + db[2] + "x" + db[3]
                + " | overlay=" + (regionOverlay != null ? regionOverlay.getWidth() + "x" + regionOverlay.getHeight() : "?")
                + " | " + (regionInfoT != null ? regionInfoT.getText() : "?"));
    }

    /** v12 调试（demo region 第二轮）：置形状记忆为「圆」→ 开第二轮（上一轮未收尾则 600ms 后重试）。 */
    private void regionDemoStartCircle() {
        if (regionAdded) {
            h.postDelayed(new Runnable() { public void run() { regionDemoStartCircle(); } }, 600);
            return;
        }
        try {
            getSharedPreferences("chip", MODE_PRIVATE).edit().putInt("regionShape", 1).apply();
        } catch (Throwable ig) { }
        startRegionPick();
    }

    /** v12 调试（demo region 第二轮）：切「圆」并置圆心/半径 + 进编辑态。 */
    private void regionDemoBoxCirc() {
        if (!regionAdded || regionIv == null || regionShot == null) return;
        if (regionShape != 1) setRegionShape(1);
        int w = regionShot.w, h = regionShot.h;
        regionIv.setCircle(Math.round(w * 0.6f), Math.round(h * 0.45f),
                Math.max(8, Math.round(Math.min(w, h) * 0.22f)));
        setRegionEditing(true);
        updateRegionInfo();
        int[] dc = regionIv.circle();
        TestBench.appendLog(this, "chip_debug.txt", "regionDemoBoxCirc " + dc[0] + "," + dc[1] + " r=" + dc[2]
                + " | " + (regionInfoT != null ? regionInfoT.getText() : "?"));
    }

    /** v12 调试（demo region）：自动建区到 scratch 文件（不碰真实 regions.conf）。 */
    private void regionDemoSave() {
        if (!regionAdded || regionShot == null) return;
        File dir = getExternalFilesDir(null);
        if (dir == null) return;
        RegionDraw.pathOverride = new File(dir, "demo-regions.conf").getAbsolutePath();
        saveRegionFromSel();
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
            panelResult.setText(v == 0 ? "取图→模板 · 抓帧→帧库 · 取色→色值 · 框区域→建区"
                    : v == 1 ? "点 [试一下] 在当前屏幕上实找一次"
                    : "点 [干跑] 评估操作视觉步（不执行触摸）");
        }
    }

    /** 主视图：四个主操作（两行）+ 最近 + 两个钻入入口。 */
    private LinearLayout buildRootView() {
        LinearLayout root = Ui.col(this);
        LinearLayout acts = Ui.row(this);
        TextView capB = actBtn("取图", new View.OnClickListener() {
            public void onClick(View v) { startTmplPick(); }          /* v9.4：冻结帧框选建模板（不回 app） */
        });
        LinearLayout.LayoutParams a1 = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        a1.rightMargin = Ui.dp(this, 8);
        acts.addView(capB, a1);
        TextView grabB = actBtn("抓帧", new View.OnClickListener() {
            public void onClick(View v) { doCapture(0, false); }      /* v9.4：留在悬浮窗（不回 app） */
        });
        acts.addView(grabB, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        acts.setPadding(0, Ui.dp(this, 2), 0, Ui.dp(this, 4));
        root.addView(acts);
        LinearLayout acts2 = Ui.row(this);
        TextView pcB = actBtn("取色", new View.OnClickListener() {
            public void onClick(View v) { startPanelColor(); }
        });
        LinearLayout.LayoutParams a3 = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        a3.rightMargin = Ui.dp(this, 8);
        acts2.addView(pcB, a3);
        TextView regB = actBtn("框区域", new View.OnClickListener() {
            public void onClick(View v) { startRegionPick(); }        /* v12：冻结帧框选建区（矩形/圆） */
        });
        acts2.addView(regB, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        acts2.setPadding(0, 0, 0, Ui.dp(this, 4));
        root.addView(acts2);
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

    /** 读 live 区域文件（干跑用；失败 null）。 */
    private static Regions.File loadRegionsFile() {
        try {
            Su.R tr = Su.run("cat " + Regions.PATH + " 2>/dev/null");
            if (!tr.ok() || tr.out == null) return null;
            return Regions.File.parse(tr.out);
        } catch (Throwable t) { return null; }
    }

    /** 区域名 → 矩形 (x,y,w,h)（口径同 OpsPage.regionRect；圆取外接矩形）——数字为基帧口径。 */
    private static Map<String, int[]> rectsOf(Regions.File rf) {
        Map<String, int[]> m = new HashMap<String, int[]>();
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
                            else {
                                Regions.File rf = loadRegionsFile();
                                rep = DryRun.run(s.rgba, s.w, s.h, s.rot,
                                        rf != null ? rf.frameRot : 0, rf != null ? rf.frameW : 0,
                                        rf != null ? rf.frameH : 0, op, rectsOf(rf));
                            }
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
        removeTmplOverlay();
        removeRegionOverlay();
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
        removeTmplOverlay();
        removeRegionOverlay();
        if (added && wm != null && chip != null) {
            try { wm.removeView(chip); } catch (Throwable ig) { }
        }
        added = false;
        h.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
