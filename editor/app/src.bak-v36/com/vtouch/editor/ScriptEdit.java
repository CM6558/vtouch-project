package com.vtouch.editor;

import android.content.Context;
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
 *  - 无自动换行（行号=文本行一一对应），编辑器内部横竖滚动。
 * 视觉口径 = app v5.1 体系（Ui 令牌/等宽/边框卡片）；纯 UI 件，逻辑在 ScriptStore。
 */
public final class ScriptEdit {

    public final LinearLayout root;
    private final Context c;
    private final EditText edit;
    private final ScrollView sv;
    private final GutterView gutter;
    private final TextView errBar;
    private final LinearLayout assistRow;

    private final int C_KW = Ui.ACCENT, C_STR = Ui.OK, C_CMT = Ui.FAINT;
    private final int C_ERR_BG = (Ui.WARN & 0x00FFFFFF) | 0x1F000000;

    private int errLine = 0;
    private String errMsg = null;
    private String lastChecked = null;
    private boolean hlPending = false, ckPending = false;
    private boolean selfEdit = false;     // v36：程序性改写（自动缩进回填）防递归
    private int lastLen = -1;             // v36：上次文本长度（识别「单字符=回车」）
    public boolean dirty = false;

    private static final String[][] KEYS = {
        { "点按", "滑动", "等待", "按下", "弹起" },
        { "如果", "否则", "循环", "当", "直到", "跳出", "继续", "终止" },
        { "提示", "框区域", "框矩形", "找图", "找色", "在区域", "开关开" },
        { "函数", "表", "返回" },
        { "缩进", "取消缩进" },
        { "(", "{", "[", "\"", "=", "#", "%" },
    };

    private ScriptEdit(Context cc, String initial, String meta, View.OnClickListener onProps) {
        c = cc;
        root = Ui.col(c);

        // ---- 元信息行：门控/自动关摘要 + [属性]（门控与写盘口径等仍走 ops.conf 元数据）----
        LinearLayout metaRow = Ui.row(c);
        metaRow.setPadding(Ui.dp(c, 8), Ui.dp(c, 2), Ui.dp(c, 8), Ui.dp(c, 2));
        TextView metaTv = Ui.mono(c, meta == null ? "" : meta, 11.5f, Ui.DIM);
        metaRow.addView(metaTv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView props = Ui.btnAux(c, "属性", Ui.ACCENT);
        props.setOnClickListener(onProps);
        metaRow.addView(props, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(metaRow, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- 代码区：行号槽 + 编辑器（卡片一体的输入面）----
        LinearLayout codeCard = Ui.row(c);
        codeCard.setBackground(Ui.strokedRound(c, Ui.CARD, Ui.CARD_LINE, 8));
        codeCard.setPadding(0, Ui.dp(c, 6), Ui.dp(c, 6), Ui.dp(c, 6));

        edit = new EditText(c);
        edit.setTextSize(13);
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
        edit.setOnTouchListener(new View.OnTouchListener() {
            public boolean onTouch(View v, android.view.MotionEvent ev) {
                if (ev.getAction() == android.view.MotionEvent.ACTION_MOVE) {
                    sv.requestDisallowInterceptTouchEvent(false);
                }
                return false;
            }
        });
        LinearLayout codeRow = Ui.row(c);
        gutter = new GutterView(c);
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

        // ---- 输入辅助条（底部横滚文字键；成对符号一次成对）----
        assistRow = Ui.row(c);
        for (int g = 0; g < KEYS.length; g++) {
            if (g > 0) {
                View div = new View(c);
                div.setBackgroundColor(Ui.CARD_LINE);
                LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(Ui.dp(c, 1), Ui.dp(c, 18));
                dlp.leftMargin = Ui.dp(c, 6);
                div.setLayoutParams(dlp);
                assistRow.addView(div);
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
                assistRow.addView(b);
            }
        }
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
                gutter.requestLayout();
                gutter.invalidate();
                edit.post(new Runnable() {
                    public void run() { ensureCursorVisible(); }
                });
                if (!hlPending) {
                    hlPending = true;
                    Ui.MAIN.postDelayed(new Runnable() {
                        public void run() { hlPending = false; highlight(); }
                    }, 90);
                }
                if (!ckPending) {
                    ckPending = true;
                    Ui.MAIN.postDelayed(new Runnable() {
                        public void run() { ckPending = false; check(); }
                    }, 300);
                }
            }
        });
        lastLen = edit.length();              // v36：初始化（首字符输入即可识别回车）
        highlight();
        check();
    }

    public static ScriptEdit build(Context c, String initial, String meta, View.OnClickListener onProps) {
        return new ScriptEdit(c, initial, meta, onProps);
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
        errLine = line;
        errMsg = msg;
        if (line > 0 || (msg != null && msg.length() > 0)) {
            errBar.setText(line > 0 ? ("第 " + line + " 行: " + msg) : msg);
            errBar.setVisibility(View.VISIBLE);
        } else {
            errBar.setVisibility(View.GONE);
        }
        highlight();
    }

    public void clearError() {
        errLine = 0;
        errMsg = null;
        errBar.setVisibility(View.GONE);
        highlight();
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
        Editable e = edit.getText();
        if (e == null) return;
        int n = e.length();
        Cspan[] cs = e.getSpans(0, n, Cspan.class);
        for (int i = 0; i < cs.length; i++) e.removeSpan(cs[i]);
        Bspan[] bs = e.getSpans(0, n, Bspan.class);
        for (int i = 0; i < bs.length; i++) e.removeSpan(bs[i]);

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
                e.setSpan(new Cspan(C_STR), st, Math.min(i, n), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else if (ch == '#') {
                int st = i;
                while (i < n && e.charAt(i) != '\n') i++;
                e.setSpan(new Cspan(C_CMT), st, i, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else if (idStart(ch)) {
                int st = i;
                while (i < n && idPart(e.charAt(i))) i++;
                String w = e.subSequence(st, i).toString();
                if (VtTok.kwOf(w) > 0) e.setSpan(new Cspan(C_KW), st, i, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else {
                i++;
            }
        }
        // 错误行底
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
        String t = edit.getText().toString();
        if (t.equals(lastChecked)) return;
        lastChecked = t;
        VtCheck.R r = VtCheck.run(t);
        if (r.err != null) showErrorQuiet(r.line, r.err);
        else showErrorQuiet(0, null);
    }

    /** 校验结果上屏。 */
    private void showErrorQuiet(int line, String msg) {
        errLine = line;
        errMsg = msg;
        if (errLine > 0 || (errMsg != null && errMsg.length() > 0)) {
            errBar.setText(errLine > 0 ? ("第 " + errLine + " 行: " + errMsg) : errMsg);
            errBar.setVisibility(View.VISIBLE);
        } else {
            errBar.setVisibility(View.GONE);
        }
        highlight();
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
        if (label.equals("(")) insertPair("(", ")");
        else if (label.equals("{")) insertPair("{", "}");
        else if (label.equals("[")) insertPair("[", "]");
        else if (label.equals("\"")) insertPair("\"", "\"");
        else if (label.equals("=")) insert("= ", "");
        else if (label.equals("#")) insert("# ", "");
        else if (label.equals("%")) insert("%", "");
        else insert(label + " ", "");
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

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int h = MeasureSpec.getSize(hSpec);
            int digits = lastDigits;
            float wnum = p.measureText("0000".substring(0, Math.max(1, digits)));
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

    public int assistKeyCountForTest() { return assistRow.getChildCount(); }

    public void insertKeyForTest(String label) { applyKey(label); }

    public void insertForTest(String s) { insert(s, ""); }

    public void indentForTest(boolean out) { indentSelection(out); }

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

    public int gutterWidthForTest() { return gutter.getMeasuredWidth(); }

    public int gutterDigitsForTest() {
        Layout lay = edit.getLayout();
        int count = lay == null ? 1 : lay.getLineCount();
        return String.valueOf(count).length();
    }
}
