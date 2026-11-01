package com.vtouch.editor;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.Editable;
import android.text.Layout;
import android.text.Spanned;
import android.text.TextWatcher;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * 脚本文本编辑器（V8 T6.2，「双视图」的脚本侧）。
 *  - 行号槽：自绘，按 EditText 的实际 Layout（getLineTop/getLineBaseline）逐行对齐——中文/等宽混排不漂移；
 *  - 语法高亮：保留字（琥珀）/字符串（绿）/注释（灰）——轻量扫描器，输入中途（未闭合）也稳定；
 *  - 错误行：VtCheck 防抖检查，错误行号 + 行底浅红 + 底栏一条可点（跳到该行）；
 *  - 输入辅助条：底部横滚文字键（语句词/符号/缩进），成对符号一次成对、光标居中；
 *  - 缩进（v36）：回车自动缩进（沿用上一行缩进；上一行以 `{` 收尾再加一级）；[缩进]/[取消缩进] 作用于当前行或选中行；
 *  - 缩放（v38）：双指捏合改代码字号（9..24sp，0.5 步进；行号槽同比例跟缩；全局记忆；不加控件）；
 *  - 取点（v39）：辅助条 [帧上取点]/[屏幕取点] → 取到的竖屏逻辑坐标插入光标处「x y 」（回填回调由 OpsPage 注入）；
 *  - 无自动换行（行号=文本行一一对应），编辑器内部横竖滚动。
 * 视觉口径 = app v5.1 体系（Ui 令牌/等宽/边框卡片）；纯 UI 件，逻辑在 ScriptStore。
 */
public final class ScriptEdit {

    private static final java.util.concurrent.ExecutorService CHECK_EXECUTOR =
            new java.util.concurrent.ThreadPoolExecutor(1, 1, 0L, java.util.concurrent.TimeUnit.MILLISECONDS,
            new java.util.concurrent.ArrayBlockingQueue<Runnable>(1), new java.util.concurrent.ThreadFactory() {
                public Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "script-check");
                    t.setDaemon(true);
                    return t;
                }
            }, new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());

    public final LinearLayout root;
    private final Context c;
    private final EditText edit;
    private final ScrollView sv;
    private final GutterView gutter;
    private final TextView errBar;
    private final TextView metaTv;
    private final LinearLayout assistRow;
    private final LinearLayout hintRow;      // v37：提示条内容行（exact 文本 或 候选键）
    private final LinearLayout hintWrap;     // v37：提示条容器（无提示收起）

    private final int C_KW = Ui.ACCENT, C_STR = Ui.OK, C_CMT = Ui.FAINT;
    private final int C_ERR_BG = (Ui.WARN & 0x00FFFFFF) | 0x1F000000;

    // ---- v38：双指捏合缩放（只加手势，不加控件）----
    public static final float ZOOM_MIN = 9f, ZOOM_MAX = 24f;
    public static final String PREF_FILE = "ui", PREF_KEY = "scriptZoomSp";
    /** 测试免写 prefs（离屏核用；套件收工须复原）。 */
    public static boolean zoomPersistSuppressedForTest = false;
    private final SharedPreferences prefs;
    private float zoomSp = 13f;            // 当前字号（sp，0.5 步进）
    private float zoomAcc = 13f;           // 捏合累计（不量化：防小步缩放被四舍五入吃掉）
    private float zoomBeginSp = 13f;       // 手势起始字号（判「变过没」）
    private int zoomAnchorLine = -1;       // 待应用的视口锚行（下次布局生效）
    private int zoomKeepLine = -1;         // 本次手势的锚行（每步改尺后重设待应用）
    private boolean pinching = false;      // 本手势已被缩放接管（吞到全抬指）
    private ScaleGestureDetector pinch;
    private boolean pinchInitFailed = false;   // 无 Looper 线程（离屏核 probe 线程）构造失败标记：放弃缩放不崩

    // ---- v39：取点回填（辅助条两枚键 → OpsPage 注入回调）----
    private Runnable onPickFrame, onPickScreen;

    private int errLine = 0;
    private String errMsg = null;
    private String lastChecked = null;
    private int checkGeneration = 0;
    private int highlightGeneration = 0;
    private static final java.util.concurrent.ExecutorService HIGHLIGHT_EXECUTOR =
            new java.util.concurrent.ThreadPoolExecutor(1, 1, 0L, java.util.concurrent.TimeUnit.MILLISECONDS,
            new java.util.concurrent.ArrayBlockingQueue<Runnable>(1), new java.util.concurrent.ThreadFactory() {
                public Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "script-highlight");
                    t.setDaemon(true);
                    return t;
                }
            }, new java.util.concurrent.ThreadPoolExecutor.DiscardOldestPolicy());
    private final Runnable highlightTask = new Runnable() {
        public void run() { highlightAsync(); }
    };
    private final Runnable checkTask = new Runnable() {
        public void run() { checkAsync(); }
    };
    private final Runnable cursorTask = new Runnable() {
        public void run() { ensureCursorVisible(); }
    };
    private boolean selfEdit = false;     // v36：程序性改写（自动缩进回填）防递归
    private int lastLen = -1;             // v36：上次文本长度（识别「单字符=回车」）
    public boolean dirty = false;

    private static final String[][] KEYS = {
        { "点按", "滑动", "等待", "按下", "弹起" },
        { "如果", "否则", "循环", "当", "直到", "跳出", "继续", "终止" },
        { "提示", "框区域", "框矩形", "找图", "找色", "在区域", "开关开" },
        { "函数", "表", "返回" },
        { "帧上取点", "屏幕取点" },        // v39：取点回填（帧会话 / 屏幕武装）
        { "缩进", "取消缩进" },
        { "(", "{", "[", "\"", "=", "#", "%" },
    };

    private ScriptEdit(Context cc, String initial, String meta, View.OnClickListener onProps) {
        c = cc;
        root = Ui.col(c);

        // v38：字号记忆（全局；读取越界/NAN 一律钳回 9..24sp）
        prefs = cc.getSharedPreferences(PREF_FILE, Context.MODE_PRIVATE);
        zoomSp = clampZoom(prefs.getFloat(PREF_KEY, 13f));
        zoomAcc = zoomSp;

        // ---- 元信息行：门控/自动关摘要 + [属性]（门控与写盘口径等仍走 ops.conf 元数据）----
        LinearLayout metaRow = Ui.row(c);
        metaRow.setPadding(Ui.dp(c, 8), Ui.dp(c, 2), Ui.dp(c, 8), Ui.dp(c, 2));
        metaTv = Ui.mono(c, meta == null ? "" : meta, 11.5f, Ui.DIM);
        metaRow.addView(metaTv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView props = Ui.btnAux(c, "属性", Ui.ACCENT);
        props.setOnClickListener(onProps);
        metaRow.addView(props, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(metaRow, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- 代码区：行号槽 + 编辑器（卡片一体的输入面）----
        PinchCard codeCard = new PinchCard(c);      // v38：卡片级派发=捏合缩放（编区/行号槽/空白通吃）
        codeCard.setBackground(Ui.strokedRound(c, Ui.CARD, Ui.CARD_LINE, 8));
        codeCard.setPadding(0, Ui.dp(c, 6), Ui.dp(c, 6), Ui.dp(c, 6));

        edit = new EditText(c) {
            @Override
            protected void onSelectionChanged(int selStart, int selEnd) {   // v37：光标移动 → 刷新提示
                super.onSelectionChanged(selStart, selEnd);
                updateHint();
            }
        };
        edit.setTextSize(zoomSp);          // v38：字号随捏合缩放（默认 13sp）
        edit.setTextColor(Ui.TEXT);
        edit.setHint("在这里写脚本…");
        edit.setHintTextColor(Ui.DIM);
        edit.setTypeface(Ui.monoFont(c));
        edit.setIncludeFontPadding(false);
        edit.setBackground(null);
        edit.setPadding(Ui.dp(c, 2), Ui.dp(c, 4), Ui.dp(c, 2), Ui.dp(c, 4));
        edit.setLineSpacing(Ui.dp(c, 4), 1f);
        edit.setHorizontallyScrolling(true);
        edit.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        edit.setHighlightColor((Ui.ACCENT & 0x00FFFFFF) | 0x59000000);
        edit.setOverScrollMode(View.OVER_SCROLL_NEVER);
        edit.setText(initial == null ? "" : initial);
        edit.setSelection(0);

        // v35：滚动模型 = 外层 ScrollView 承载（拖拽=滚动，标准手感）；横向滚动仍走 EditText 内部。
        // 触摸重允许拦截：EditText 拖拽默认走选字（会吞掉父级滚动）——MOVE 时放行让本层接管（社区公认修法）。
        sv = new ScrollView(c);
        sv.setOverScrollMode(View.OVER_SCROLL_NEVER);
        // v35 手势修法保持原样：EditText 拖拽默认走选字（会吞掉父级滚动）——MOVE 时放行让本层接管。
        // v38：双指捏合＝缩放由「代码卡片」派发层接管（见 PinchCard），此处不再参与缩放判定。
        edit.setOnTouchListener(new View.OnTouchListener() {
            public boolean onTouch(View v, android.view.MotionEvent ev) {
                if (ev.getAction() == android.view.MotionEvent.ACTION_MOVE) {
                    sv.requestDisallowInterceptTouchEvent(false);
                }
                return false;
            }
        });
        // 字号变化 → 下一次布局按「手势起点所在行」重锚视口（行数不随字号变，锚行号恒有效）。
        edit.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int orr, int ob) {
                if (zoomAnchorLine < 0) return;
                int ln = zoomAnchorLine;
                zoomAnchorLine = -1;
                Layout lay = edit.getLayout();
                if (lay == null) return;
                sv.scrollTo(0, Math.max(0, lay.getLineTop(Math.min(ln, lay.getLineCount() - 1))));
                gutter.invalidate();
            }
        });
        LinearLayout codeRow = Ui.row(c);
        gutter = new GutterView(c);
        gutter.setFontSp(zoomSp);
        codeRow.addView(gutter, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT));
        codeRow.addView(edit, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        sv.addView(codeRow, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        codeCard.addView(sv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0);
        clp.weight = 1f;
        clp.setMargins(Ui.dp(c, 8), Ui.dp(c, 4), Ui.dp(c, 8), 0);
        root.addView(codeCard, clp);

        // ---- 错误条（隐藏态零高度；可点跳行）----
        errBar = Ui.tv(c, "", 12, Ui.WARN, false);
        errBar.setMaxLines(2);
        errBar.setBackground(Ui.round(c, C_ERR_BG, 6));
        errBar.setPadding(Ui.dp(c, 10), Ui.dp(c, 5), Ui.dp(c, 10), Ui.dp(c, 5));
        errBar.setVisibility(View.GONE);
        errBar.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { scrollToLine(errLine); }
        });
        LinearLayout.LayoutParams elp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        elp.setMargins(Ui.dp(c, 8), Ui.dp(c, 6), Ui.dp(c, 8), 0);
        root.addView(errBar, elp);

        // ---- 输入提示条（v37：命令作用+参数 / 前缀补全候选；无提示收起）----
        hintRow = Ui.row(c);
        hintWrap = Ui.row(c);
        hintWrap.setBackground(Ui.strokedRound(c, Ui.CARD, Ui.CARD_LINE, 8));
        hintWrap.setPadding(Ui.dp(c, 6), Ui.dp(c, 1), Ui.dp(c, 6), Ui.dp(c, 1));
        HorizontalScrollView hhs = new HorizontalScrollView(c);
        hhs.setHorizontalScrollBarEnabled(false);
        hhs.addView(hintRow);
        hintWrap.addView(hhs, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        hintWrap.setVisibility(View.GONE);
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hlp.setMargins(Ui.dp(c, 8), Ui.dp(c, 6), Ui.dp(c, 8), 0);
        root.addView(hintWrap, hlp);

        // ---- 输入辅助条（底部横滚文字键；成对符号一次成对）----
        assistRow = Ui.row(c);
        final LinearLayout moreKeys = Ui.row(c);
        TextView moreButton = Ui.btnFlat(c, "更多 ▾", Ui.ACCENT);
        moreButton.setContentDescription("展开或收起更多脚本输入辅助键");
        for (int g = 0; g < KEYS.length; g++) {
            LinearLayout target = g == 0 ? assistRow : moreKeys;
            if (g > 0) {
                View div = new View(c);
                div.setBackgroundColor(Ui.CARD_LINE);
                LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(Ui.dp(c, 1), Ui.dp(c, 18));
                dlp.leftMargin = Ui.dp(c, 6);
                div.setLayoutParams(dlp);
                target.addView(div);
            }
            for (int k = 0; k < KEYS[g].length; k++) {
                final String label = KEYS[g][k];
                boolean sym = g == KEYS.length - 1;
                TextView b = Ui.btnFlat(c, label, sym ? Ui.DIM : Ui.TEXT);
                b.setTextSize(12.5f);
                b.setMinimumWidth(Ui.dp(c, sym ? 40 : 48));
                b.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { applyKey(label); }
                });
                target.addView(b);
            }
        }
        assistRow.addView(moreButton);
        moreKeys.setVisibility(View.GONE);
        assistRow.addView(moreKeys);
        moreButton.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                boolean show = moreKeys.getVisibility() != View.VISIBLE;
                moreKeys.setVisibility(show ? View.VISIBLE : View.GONE);
                moreButton.setText(show ? "收起 ▴" : "更多 ▾");
            }
        });
        HorizontalScrollView hs = new HorizontalScrollView(c);
        hs.setHorizontalScrollBarEnabled(false);
        hs.addView(assistRow);
        LinearLayout assistWrap = Ui.row(c);
        assistWrap.setBackground(Ui.strokedRound(c, Ui.CARD, Ui.CARD_LINE, 8));
        assistWrap.setPadding(Ui.dp(c, 6), Ui.dp(c, 2), Ui.dp(c, 6), Ui.dp(c, 2));
        assistWrap.addView(hs, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        alp.setMargins(Ui.dp(c, 8), Ui.dp(c, 6), Ui.dp(c, 8), Ui.dp(c, 8));
        root.addView(assistWrap, alp);

        // ---- 输入监听：高亮防抖 + 校验防抖 ----
        edit.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int st, int cn, int af) {}
            public void onTextChanged(CharSequence s, int st, int bf, int cn) {}
            public void afterTextChanged(Editable s) {
                if (selfEdit) {
                    lastLen = s.length();
                    return;
                }
                if (!selfEdit) {
                    int len = s.length();
                    if (lastLen >= 0 && len == lastLen + 1) {
                        int pos = edit.getSelectionStart();
                        if (pos > 0 && pos <= len && s.charAt(pos - 1) == '\n') {
                            String add = autoIndent(s, pos - 1);
                            if (add.length() > 0) {
                                selfEdit = true;
                                s.insert(pos, add);
                                edit.setSelection(pos + add.length());
                                selfEdit = false;
                            }
                        }
                    }
                    lastLen = s.length();
                } else {
                    lastLen = s.length();
                }
                dirty = true;
                checkGeneration++;
                highlightGeneration++;
                gutter.requestLayout();
                gutter.invalidate();
                edit.removeCallbacks(cursorTask);
                edit.post(cursorTask);
                Ui.MAIN.removeCallbacks(highlightTask);
                Ui.MAIN.postDelayed(highlightTask, 90);
                Ui.MAIN.removeCallbacks(checkTask);
                Ui.MAIN.postDelayed(checkTask, 300);
                updateHint();
            }
        });
        lastLen = edit.length();              // v36：初始化（首字符输入即可识别回车）
        highlight();
        check();
        updateHint();                         // v37：初态提示（无则收起）
    }

    public static ScriptEdit build(Context c, String initial, String meta, View.OnClickListener onProps) {
        return new ScriptEdit(c, initial, meta, onProps);
    }

    public void setMetaSummary(String meta) { metaTv.setText(meta); }

    /** v39：给辅助条「帧上取点/屏幕取点」挂回调（同 Forms.Num.enablePick 口径；未挂时点击仅提示）。 */
    public void enablePickKeys(Runnable frame, Runnable screen) {
        onPickFrame = frame;
        onPickScreen = screen;
    }

    /** v39：取点回填——在光标处插入「x y 」（尾随空格：续写参数不断词）。 */
    public void insertPickedPoint(int x, int y) {
        insert(x + " " + y + " ", "");
    }

    // ================= v38：捏合缩放 =================

    /** 钳制（未量化；NAN/越界回默认 13）。 */
    private static float clampZoomRaw(float sp) {
        if (Float.isNaN(sp)) return 13f;
        if (sp < ZOOM_MIN) return ZOOM_MIN;
        if (sp > ZOOM_MAX) return ZOOM_MAX;
        return sp;
    }

    /** 0.5sp 步进（连续捏合落到半号，避免半像素抖动）。 */
    private static float quantZoom(float sp) { return Math.round(sp * 2f) / 2f; }

    private static float clampZoom(float sp) { return quantZoom(clampZoomRaw(sp)); }

    private static String fmtSp(float sp) {
        return sp == Math.floor(sp) ? String.valueOf((int) sp) : String.valueOf(sp);
    }

    /** 套用字号：代码文本 + 行号槽同比例（视口重锚由布局监听完成）。 */
    private void applyZoomSp(float sp) {
        edit.setTextSize(sp);
        gutter.setFontSp(sp);
        gutter.invalidate();
    }

    /** 手势结束：落盘记忆（受测试抑制闸）+ 轻提示「字号 N」。 */
    private void commitZoom() {
        if (!zoomPersistSuppressedForTest) {
            try { prefs.edit().putFloat(PREF_KEY, zoomSp).apply(); } catch (Throwable ig) { }
        }
        Ui.toast("字号 " + fmtSp(zoomSp));
    }

    /** v38：捏合探测器惰性构造——其内部会 new Handler，要求带 Looper 的线程（真实触摸必达主线程）；
     *  离屏核的 probe 线程无 Looper，构造会抛「Can't create handler」——故不在此构造。 */
    private void ensurePinch() {
        if (pinch != null || pinchInitFailed) return;
        try {
            pinch = new ScaleGestureDetector(c, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                @Override public boolean onScaleBegin(ScaleGestureDetector d) {
                    zoomBeginSp = zoomSp;
                    zoomAcc = zoomSp;
                    Layout lay = edit.getLayout();
                    zoomKeepLine = lay == null ? 0 : lay.getLineForVertical(sv.getScrollY());
                    return true;
                }
                @Override public boolean onScale(ScaleGestureDetector d) {
                    zoomAcc = clampZoomRaw(zoomAcc * d.getScaleFactor());   // 累计不量化
                    float sp = quantZoom(zoomAcc);
                    if (sp != zoomSp) {
                        zoomSp = sp;
                        zoomAnchorLine = zoomKeepLine;                      // 本步改尺后按手势锚行重定位
                        applyZoomSp(sp);
                    }
                    return true;
                }
                @Override public void onScaleEnd(ScaleGestureDetector d) {
                    zoomAcc = zoomSp;
                    zoomKeepLine = -1;
                    if (zoomSp != zoomBeginSp) commitZoom();
                }
            });
        } catch (Throwable t) {
            pinchInitFailed = true;
        }
    }

    // ================= 数据面 =================

    public String text() { return edit.getText().toString(); }

    public void setText(String s) {
        edit.setText(s == null ? "" : s);
        edit.setSelection(0);
        highlight();
        check();
    }

    public int errorLine() { return errLine; }

    public String errorMsg() { return errMsg; }

    /** 错误态：行号 + 文案（line<=0 表示无行号错误）。 */
    public void showError(int line, String msg) {
        checkGeneration++;
        Ui.MAIN.removeCallbacks(checkTask);
        errLine = line;
        errMsg = msg;
        if (line > 0 || (msg != null && msg.length() > 0)) {
            errBar.setText(line > 0 ? ("第 " + line + " 行: " + msg) : msg);
            errBar.setVisibility(View.VISIBLE);
        } else {
            errBar.setVisibility(View.GONE);
        }
        updateErrorHighlight();
    }

    public void clearError() {
        checkGeneration++;
        Ui.MAIN.removeCallbacks(checkTask);
        errLine = 0;
        errMsg = null;
        errBar.setVisibility(View.GONE);
        updateErrorHighlight();
    }

    /** 滚动到行（1 基），行顶留 30dp 视距。 */
    public void scrollToLine(int line) {
        Layout lay = edit.getLayout();
        if (lay == null || line <= 0) return;
        int idx = Math.min(line - 1, lay.getLineCount() - 1);
        int y = lay.getLineTop(Math.max(0, idx));
        sv.scrollTo(0, Math.max(0, y - Ui.dp(c, 30)));
        gutter.invalidate();
    }

    /** 光标行保持可见（外层 ScrollView 不会自动跟随光标 → 打字/吸入/助手插入后手动跟随）。 */
    private void ensureCursorVisible() {
        Layout lay = edit.getLayout();
        if (lay == null) return;
        int sel = edit.getSelectionStart();
        if (sel < 0) sel = edit.getSelectionEnd();
        if (sel < 0 || sel > edit.length()) return;
        int line = lay.getLineForOffset(sel);
        int top = lay.getLineTop(line);
        int bot = lay.getLineBottom(line);
        int sy = sv.getScrollY();
        int vh = sv.getHeight() - Ui.dp(c, 8);
        if (vh <= 0) return;
        if (bot > sy + vh) sv.smoothScrollTo(0, bot - vh + Ui.dp(c, 16));
        else if (top < sy) sv.smoothScrollTo(0, Math.max(0, top - Ui.dp(c, 16)));
    }

    // ================= 高亮 / 校验 =================

    private void highlight() {
        highlightGeneration++;
        Ui.MAIN.removeCallbacks(highlightTask);
        Editable e = edit.getText();
        if (e == null) return;
        applyHighlight(e, scanHighlight(e.toString()));
    }

    private static final class HighlightRange {
        final int start, end, kind;
        HighlightRange(int start, int end, int kind) {
            this.start = start; this.end = end; this.kind = kind;
        }
    }

    private void highlightAsync() {
        final String text = edit.getText().toString();
        final int generation = ++highlightGeneration;
        HIGHLIGHT_EXECUTOR.execute(new Runnable() {
            public void run() {
                final java.util.List<HighlightRange> ranges = scanHighlight(text);
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (generation != highlightGeneration) return;
                        applyHighlight(edit.getText(), ranges);
                    }
                });
            }
        });
    }

    private void applyHighlight(Editable e, java.util.List<HighlightRange> ranges) {
        int n = e.length();
        Cspan[] cs = e.getSpans(0, n, Cspan.class);
        for (int i = 0; i < cs.length; i++) e.removeSpan(cs[i]);
        for (HighlightRange r : ranges) {
            int color = r.kind == 1 ? C_STR : r.kind == 2 ? C_CMT : C_KW;
            e.setSpan(new Cspan(color), r.start, r.end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        updateErrorHighlight();
    }

    private static java.util.List<HighlightRange> scanHighlight(String e) {
        java.util.List<HighlightRange> ranges = new java.util.ArrayList<HighlightRange>();
        int n = e.length();
        int i = 0;
        while (i < n) {
            char ch = e.charAt(i);
            if (ch == '"') {
                int st = i;
                i++;
                while (i < n) {
                    char x = e.charAt(i);
                    if (x == '\\') { i += 2; continue; }
                    if (x == '"' || x == '\n') break;
                    i++;
                }
                if (i < n && e.charAt(i) == '"') i++;
                ranges.add(new HighlightRange(st, Math.min(i, n), 1));
            } else if (ch == '#') {
                int st = i;
                while (i < n && e.charAt(i) != '\n') i++;
                ranges.add(new HighlightRange(st, i, 2));
            } else if (idStart(ch)) {
                int st = i;
                while (i < n && idPart(e.charAt(i))) i++;
                String w = e.subSequence(st, i).toString();
                if (VtTok.kwOf(w) > 0) ranges.add(new HighlightRange(st, i, 3));
            } else {
                i++;
            }
        }
        return ranges;
    }

    private void updateErrorHighlight() {
        Editable e = edit.getText();
        if (e == null) return;
        Bspan[] bs = e.getSpans(0, e.length(), Bspan.class);
        for (int i = 0; i < bs.length; i++) e.removeSpan(bs[i]);
        if (errLine > 0) {
            int[] r = lineRange(e, errLine);
            if (r != null) e.setSpan(new Bspan(C_ERR_BG), r[0], r[1], Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        gutter.invalidate();
    }

    private int[] lineRange(CharSequence e, int line) {
        int n = e.length();
        int ln = 1, st = 0;
        for (int i = 0; i <= n; i++) {
            if (i == n || e.charAt(i) == '\n') {
                if (ln == line) return new int[]{ st, i };
                ln++;
                st = i + 1;
            }
        }
        return null;
    }

    private void check() {
        checkGeneration++;
        String t = edit.getText().toString();
        if (t.equals(lastChecked)) return;
        lastChecked = t;
        VtCheck.R r = VtCheck.run(t);
        if (r.err != null) showErrorQuiet(r.line, r.err);
        else showErrorQuiet(0, null);
    }

    private void checkAsync() {
        final String text = edit.getText().toString();
        if (text.equals(lastChecked)) return;
        final int generation = ++checkGeneration;
        CHECK_EXECUTOR.execute(new Runnable() {
            public void run() {
                final VtCheck.R result = VtCheck.run(text);
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (generation != checkGeneration) return;
                        lastChecked = text;
                        showErrorQuiet(result.err == null ? 0 : result.line, result.err);
                    }
                });
            }
        });
    }

    /** 校验结果上屏。 */
    private void showErrorQuiet(int line, String msg) {
        boolean changed = errLine != line || (errMsg == null ? msg != null : !errMsg.equals(msg));
        errLine = line;
        errMsg = msg;
        if (errLine > 0 || (errMsg != null && errMsg.length() > 0)) {
            errBar.setText(errLine > 0 ? ("第 " + errLine + " 行: " + errMsg) : errMsg);
            errBar.setVisibility(View.VISIBLE);
        } else {
            errBar.setVisibility(View.GONE);
        }
        if (changed) updateErrorHighlight();
    }

    private static boolean idStart(char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch > 0x7F;
    }

    private static boolean idPart(char ch) {
        return idStart(ch) || (ch >= '0' && ch <= '9');
    }

    static final class Cspan extends ForegroundColorSpan {
        Cspan(int color) { super(color); }
    }

    static final class Bspan extends BackgroundColorSpan {
        Bspan(int color) { super(color); }
    }

    // ================= 输入辅助 =================

    private void applyKey(String label) {
        if (label.equals("缩进")) { indentSelection(false); return; }
        if (label.equals("取消缩进")) { indentSelection(true); return; }
        if (label.equals("帧上取点")) {              // v39：取点键走注入回调（编辑器内由 OpsPage 提供）
            if (onPickFrame != null) onPickFrame.run(); else Ui.toast("取点仅在操作编辑页可用");
            return;
        }
        if (label.equals("屏幕取点")) {
            if (onPickScreen != null) onPickScreen.run(); else Ui.toast("取点仅在操作编辑页可用");
            return;
        }
        if (label.equals("(")) insertPair("(", ")");
        else if (label.equals("{")) insertPair("{", "}");
        else if (label.equals("[")) insertPair("[", "]");
        else if (label.equals("\"")) insertPair("\"", "\"");
        else if (label.equals("=")) insert("= ", "");
        else if (label.equals("#")) insert("# ", "");
        else if (label.equals("%")) insert("%", "");
        else {
            VtHelp.H h = VtHelp.of(label);            // v37b：命令键 = 插入参数模板
            if (h != null) insertTemplate(h, false);
            else insert(label + " ", "");
        }
    }

    private void insertPair(String o, String cl) {
        insert(o, cl);
    }

    private void insert(String before, String after) {
        int st = edit.getSelectionStart();
        int en = edit.getSelectionEnd();
        if (st < 0 || en < 0) { st = en = edit.length(); }
        if (en < st) { int t = st; st = en; en = t; }
        Editable e = edit.getText();
        e.replace(st, en, before + after);
        edit.setSelection(st + before.length());
        edit.requestFocus();
        edit.post(new Runnable() {
            public void run() { ensureCursorVisible(); }
        });
    }

    /** v36 回车自动缩进：沿用上一行缩进；上一行以 `{` 收尾再加一级（4 空格）。 */
    private String autoIndent(CharSequence s, int nlPos) {
        int st = nlPos;
        while (st > 0 && s.charAt(st - 1) != '\n') st--;
        StringBuilder ws = new StringBuilder();
        int i = st;
        while (i < nlPos && (s.charAt(i) == ' ' || s.charAt(i) == '\t')) { ws.append(s.charAt(i)); i++; }
        int e = nlPos - 1;
        while (e >= st && (s.charAt(e) == ' ' || s.charAt(e) == '\t')) e--;
        if (e >= st && s.charAt(e) == '{') ws.append("    ");
        return ws.toString();
    }

    /** v36 缩进/反缩进（当前行或选中行；空白不进语义——块结构由花括号定）。 */
    public void indentSelection(boolean out) {
        Editable e = edit.getText();
        int a = edit.getSelectionStart(), b = edit.getSelectionEnd();
        if (a < 0 || b < 0) { a = b = e.length(); }
        if (b < a) { int t = a; a = b; b = t; }
        int ls = a;
        while (ls > 0 && e.charAt(ls - 1) != '\n') ls--;
        java.util.ArrayList<Integer> starts = new java.util.ArrayList<Integer>();
        starts.add(ls);
        for (int i = ls; i < b && i < e.length(); i++) {
            if (e.charAt(i) == '\n') starts.add(i + 1);
        }
        int cs = a, ce = b;
        for (int k = starts.size() - 1; k >= 0; k--) {
            int s0 = starts.get(k);
            if (s0 > e.length()) continue;
            if (out) {
                int rm = 0;
                while (rm < 4 && s0 + rm < e.length()) {
                    char ch = e.charAt(s0 + rm);
                    if (ch != ' ' && ch != '\t') break;
                    rm++;
                }
                if (rm > 0) {
                    e.delete(s0, s0 + rm);
                    if (cs >= s0 + rm) cs -= rm; else if (cs > s0) cs = s0;
                    if (ce >= s0 + rm) ce -= rm; else if (ce > s0) ce = s0;
                }
            } else {
                e.insert(s0, "    ");
                if (cs >= s0) cs += 4;
                if (ce >= s0) ce += 4;
            }
        }
        if (cs > ce) cs = ce;
        edit.setSelection(Math.max(0, Math.min(cs, e.length())), Math.max(0, Math.min(ce, e.length())));
        edit.requestFocus();
        edit.post(new Runnable() {
            public void run() { ensureCursorVisible(); }
        });
    }

    /** v36 外部改文（[格式化]）后尽量保持视口与光标。 */
    public void setTextPreservingView(String s) {
        int sy = sv.getScrollY();
        int sel = edit.getSelectionStart();
        selfEdit = true;
        edit.setText(s == null ? "" : s);
        selfEdit = false;
        dirty = true;
        checkGeneration++;
        highlightGeneration++;
        lastLen = edit.length();
        edit.setSelection(Math.max(0, Math.min(sel < 0 ? 0 : sel, edit.length())));
        sv.scrollTo(0, sy);
        highlight();
        lastChecked = null;
        check();
        gutter.requestLayout();
        gutter.invalidate();
        edit.post(new Runnable() {
            public void run() { ensureCursorVisible(); }
        });
    }

    // ================= v38：捏合派发层 =================

    /**
     * 代码卡片 = 捏合缩放派发层：
     *  - 每笔触摸先经此派发——第二指落下即接管本手势（吞到全抬指），缩放对「编区/行号槽/空白」一视同仁；
     *  - 接管瞬间给子层补发 CANCEL（干净收场：不留半截选字）；
     *  - 单指触摸原样下传：选字/光标/拖拽滚动等一切既有行为不变。
     */
    private final class PinchCard extends LinearLayout {
        PinchCard(Context cc) {
            super(cc);
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
        }

        @Override
        public boolean dispatchTouchEvent(MotionEvent ev) {
            ensurePinch();
            if (pinch != null) pinch.onTouchEvent(ev);
            int a = ev.getActionMasked();
            if (ev.getPointerCount() >= 2 && !pinching) {
                pinching = true;
                try {
                    MotionEvent cancel = MotionEvent.obtain(ev);
                    cancel.setAction(MotionEvent.ACTION_CANCEL);
                    super.dispatchTouchEvent(cancel);
                    cancel.recycle();
                } catch (Throwable ig) { }
            }
            if (pinching) {
                if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) pinching = false;
                return true;
            }
            return super.dispatchTouchEvent(ev);
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            return true;                    // 兜底：锚定触摸流（空白未被下层消费时也能收到后续事件做捏合判定）
        }
    }

    // ================= 行号槽 =================
    private final class GutterView extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint line = new Paint();
        private int lastDigits = 2;

        GutterView(Context cc) {
            super(cc);
            p.setTypeface(Ui.monoFont(cc));
            p.setTextSize(11 * cc.getResources().getDisplayMetrics().scaledDensity);
            p.setTextAlign(Paint.Align.RIGHT);
            line.setColor(Ui.CARD_LINE);
        }

        /** v38：字号跟缩（比例 = 11:13 原口径；缩放时行号与代码同步变大/变小）。 */
        void setFontSp(float codeSp) {
            p.setTextSize((codeSp * 11f / 13f) * c.getResources().getDisplayMetrics().scaledDensity);
            requestLayout();
            invalidate();
        }

        int fontPxForTest() { return (int) p.getTextSize(); }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int h = MeasureSpec.getSize(hSpec);
            int digits = lastDigits;
            StringBuilder sample = new StringBuilder();
            for (int i = 0; i < Math.max(1, digits); i++) sample.append('0');
            float wnum = p.measureText(sample.toString());
            int w = (int) (wnum + Ui.dp(c, 14));
            setMeasuredDimension(w, h);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            Layout lay = edit.getLayout();
            if (lay == null) return;
            int scrollY = edit.getScrollY();
            int w = getWidth();
            int h = getHeight();
            int count = lay.getLineCount();
            String digits = String.valueOf(count);
            if (digits.length() != lastDigits) {
                lastDigits = digits.length();
                requestLayout();
            }
            for (int i = 0; i < count; i++) {
                int base = lay.getLineBaseline(i) - scrollY;
                if (base < -Ui.dp(c, 10) || base > h + Ui.dp(c, 10)) continue;
                p.setColor(i + 1 == errLine ? Ui.WARN : Ui.FAINT);
                canvas.drawText(String.valueOf(i + 1), w - Ui.dp(c, 7), base, p);
            }
            canvas.drawRect(w - 1, 0, w, h, line);
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            return false;   // 行号槽不参与触摸
        }
    }

    // ================= 测试钩子（设备测试台「T6 双视图」用） =================

    public View editViewForTest() { return edit; }

    public String textForTest() { return text(); }

    public void setTextForTest(String s) { setText(s); }

    /** 同步跑一遍高亮+校验，返回错误行号（0=无错）。 */
    public int refreshNowForTest() {
        highlight();
        lastChecked = null;
        check();
        return errLine;
    }

    public int errorLineForTest() { return errLine; }

    public String errorMsgForTest() { return errMsg; }

    public int kwSpanCountForTest() {
        Editable e = edit.getText();
        int n = 0;
        Cspan[] cs = e.getSpans(0, e.length(), Cspan.class);
        for (int i = 0; i < cs.length; i++) if (cs[i].getForegroundColor() == C_KW) n++;
        return n;
    }

    public int strSpanCountForTest() {
        Editable e = edit.getText();
        int n = 0;
        Cspan[] cs = e.getSpans(0, e.length(), Cspan.class);
        for (int i = 0; i < cs.length; i++) if (cs[i].getForegroundColor() == C_STR) n++;
        return n;
    }

    public int errBgSpanCountForTest() {
        Editable e = edit.getText();
        return e.getSpans(0, e.length(), Bspan.class).length;
    }

    public int assistKeyCountForTest() {
        int count = 0;
        for (String[] group : KEYS) count += group.length;
        return count;
    }

    public void insertKeyForTest(String label) { applyKey(label); }

    public void insertForTest(String s) { insert(s, ""); }

    public void indentForTest(boolean out) { indentSelection(out); }

    // ---- v37：输入提示（命令作用+参数 / 前缀候选补全）----

    /** 刷新提示条：exact → 一行「词 · 作用 · 参数」；cands → 可点补全键；无 → 收起。 */
    private void updateHint() {
        if (hintRow == null || hintWrap == null) return;
        VtHelp.Hint h = VtHelp.hint(text(), edit.getSelectionStart());
        hintRow.removeAllViews();
        if (h == null) {
            hintWrap.setVisibility(View.GONE);
            return;
        }
        if (h.exact != null) {
            TextView tv = Ui.tv(c, "", 12, Ui.DIM, false);
            String full = h.exact.name + " · " + h.exact.what + " · " + h.exact.args;
            android.text.SpannableString sp = new android.text.SpannableString(full);
            sp.setSpan(new android.text.style.ForegroundColorSpan(Ui.ACCENT), 0, h.exact.name.length(),
                    android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            tv.setText(sp);
            tv.setMaxLines(1);
            tv.setEllipsize(android.text.TextUtils.TruncateAt.END);
            tv.setPadding(Ui.dp(c, 4), 0, Ui.dp(c, 4), 0);
            hintRow.addView(tv);
        } else if (h.cands != null) {
            for (int i = 0; i < h.cands.size(); i++) {
                final VtHelp.H cand = h.cands.get(i);
                TextView b = Ui.btnFlat(c, cand.name, Ui.TEXT);
                b.setTextSize(12.5f);
                b.setMinimumWidth(Ui.dp(c, 44));
                b.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { insertTemplate(cand, true); }
                });
                hintRow.addView(b);
            }
        }
        hintWrap.setVisibility(View.VISIBLE);
    }

    /** 前缀补全（v37b）：以**参数模板**替换光标下词（左邻 + 右延）；块级模板按当前行缩进落体。 */
    private void insertTemplate(VtHelp.H h, boolean wholeWord) {
        String t = text();
        int a = edit.getSelectionStart();
        int b = edit.getSelectionEnd();
        if (a < 0 || b < 0) { a = b = t.length(); }
        if (b < a) { int x = a; a = b; b = x; }
        int st = a, en = b;
        if (wholeWord) {
            String w = VtHelp.wordBefore(t, a);
            st = a - w.length();
            while (en < t.length() && (t.charAt(en) == '_' || Character.isLetterOrDigit(t.charAt(en)) || t.charAt(en) >= 128)) en++;
        }
        String ind = VtHelp.lineIndent(t, st);
        VtHelp.Tpl tp = VtHelp.template(h, ind);
        selfEdit = true;
        edit.getText().replace(st, en, tp.text);
        edit.setSelection(st + tp.selA, st + tp.selB);
        selfEdit = false;
        updateHint();
        edit.requestFocus();
        edit.post(new Runnable() {
            public void run() { ensureCursorVisible(); }
        });
    }

    /** v37 测试读取：提示条可见时返回其文字（exact 态）；不可见或候选态返回 null。 */
    public String hintTextForTest() {
        if (hintWrap.getVisibility() != View.VISIBLE || hintRow.getChildCount() == 0) return null;
        View v = hintRow.getChildAt(0);
        if (!(v instanceof TextView)) return null;
        CharSequence cs = ((TextView) v).getText();
        return cs == null ? null : cs.toString();
    }

    /** v37 测试读取：候选键数量（提示条不可见 = 0）。 */
    public int hintCandCountForTest() {
        if (hintWrap.getVisibility() != View.VISIBLE) return 0;
        return hintRow.getChildCount();
    }

    /** v37 测试驱动：点击第 i 个候选（模拟补全）。 */
    public void tapCandForTest(int i) {
        if (i >= 0 && i < hintRow.getChildCount()) hintRow.getChildAt(i).performClick();
    }

    /** v37 测试驱动：移动光标。 */
    public void cursorToForTest(int pos) {
        edit.setSelection(Math.max(0, Math.min(pos, edit.length())));
    }

    /** v37b 测试读取：当前选区 "a:b"。 */
    public String selForTest() { return edit.getSelectionStart() + ":" + edit.getSelectionEnd(); }

    /** 模拟单字符按键（会走自动缩进路径）；返回插入后的全文。 */
    public String typeProbeForTest(String ins) {
        int st = edit.getSelectionStart();
        if (st < 0) st = edit.length();
        edit.getText().insert(st, ins);
        return text();
    }

    public void cursorEndForTest() { edit.setSelection(edit.length()); }

    public int scrollYForTest() { return sv.getScrollY(); }

    public void scrollByForTest(int dy) { sv.scrollBy(0, dy); }

    public boolean scrollableForTest() { return sv.canScrollVertically(1); }

    // ---- v38：缩放 ----

    /** 直驱缩放（与手势同一套 clamp/quant/套用/落盘路径）。 */
    public void zoomByForTest(float factor) {
        zoomAnchorLine = -1;
        zoomKeepLine = -1;
        zoomSp = clampZoom(zoomSp * factor);
        applyZoomSp(zoomSp);
        if (!zoomPersistSuppressedForTest) {
            try { prefs.edit().putFloat(PREF_KEY, zoomSp).apply(); } catch (Throwable ig) { }
        }
    }

    public float fontSizeSpForTest() { return zoomSp; }

    public int gutterFontPxForTest() { return gutter.fontPxForTest(); }

    public int gutterWidthForTest() { return gutter.getMeasuredWidth(); }

    public int gutterDigitsForTest() {
        Layout lay = edit.getLayout();
        int count = lay == null ? 1 : lay.getLineCount();
        return String.valueOf(count).length();
    }
}
