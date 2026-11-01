package com.vtouch.editor;

import android.content.Context;
import android.text.Editable;
import android.text.InputFilter;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 步骤编辑页（全屏表单）：11 类步骤（点按/滑动/等待/按下/弹起/区域判断/开关判断/跳转/计算/找图/找色）。
 * v6 对齐仓库 ops v5 / 契约 v10：计算步命名变量 + 表达式（Expr 与核心同源校验）、找图/找色档位与持续超时、
 * 滑动两段停顿（j1/j2）、变量选择器含自定义变量（-10..-25）。
 */
public final class StepEdit {
    /** 视觉素材入口（OpsPage 实现；离屏渲染传 null 隐藏入口）。 */
    public interface Visual {
        void newTmpl(Forms.Chips field);
        void editTmpl(String name);
        void testTmpl(String name, int thresh, boolean colorCheck, String region);
        void newPts(Forms.Chips field);
        void editPts(String name);
        void testPts(String name, String region);
        void pickPointXY(Forms.Num x, Forms.Num y);
        void pickColor(ColorSink sink);
    }

    /** 吸色回填接收端。 */
    public interface ColorSink { void onColor(int rgb); }

    private static final String[] TYPE_NAMES =
            {"点按", "滑动", "等待", "按下", "弹起", "区域判断", "开关判断", "跳转", "计算", "找图", "找色"};

    public final LinearLayout root;

    private final Context c;
    private final Ops.Step work;
    private final List<String> regionIds;
    private final List<String> switchIds;
    private final List<String> tmplNames;
    private final List<String> ptsNames;
    private final String[] varSlots;      // 当前操作变量槽（含本步原值；供「同名复用/空槽分配」）
    private final Visual visual;          // 视觉素材入口（可空）
    private int curType;

    private final TextView[] typeChips = new TextView[TYPE_NAMES.length];
    private final LinearLayout dyn;
    private android.widget.ImageView refThumb;

    private Forms.Num nA1, nA2, nA3, nA4, nMs, nJ1, nJ2;
    private Forms.Num cR, cG, cB, cTol;
    private Forms.Text refT, calcName;
    private Forms.Chips refC, regC;
    private Forms.Text regT;
    private Forms.Seg tierA4, tierA3, colorMode;
    private LinearLayout j1Wrap, j2Wrap, singleWrap, multiWrap;
    private EditText exprE;
    private TextView calcErr;
    private Forms.SwitchRow chkRow;
    private View colorPrev;
    private String refLabel = "区域 id";

    private static final class Bad extends RuntimeException {
        Bad() { super("bad-field"); }
    }

    private StepEdit(Context c, Ops.Step work, List<String> regionIds, List<String> switchIds,
                     List<String> tmplNames, List<String> ptsNames, String[] varSlots, Visual visual) {
        this.c = c;
        this.work = work;
        this.regionIds = regionIds;
        this.switchIds = switchIds;
        this.tmplNames = tmplNames;
        this.ptsNames = ptsNames;
        this.varSlots = varSlots != null ? varSlots : new String[Ops.VAR_N];
        this.visual = visual;
        curType = work.type;
        root = Ui.col(c);
        root.setPadding(Ui.dp(c, 12), Ui.dp(c, 0), Ui.dp(c, 12), Ui.dp(c, 12));

        // ---- 类型宫格（3 行 × 4） ----
        LinearLayout typeCard = Forms.section(c, "步骤类型", root);
        LinearLayout[] rows = new LinearLayout[3];
        for (int r = 0; r < 3; r++) rows[r] = Ui.row(c);
        for (int i = 0; i < TYPE_NAMES.length; i++) {
            final int t = i + 1;
            TextView chip = Ui.tv(c, TYPE_NAMES[i], 12.5f, Ui.DIM, false);
            chip.setGravity(Gravity.CENTER);
            chip.setPadding(0, Ui.dp(c, 11), 0, Ui.dp(c, 11));
            chip.setClickable(true);
            chip.setFocusable(true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            if (i % 4 != 3) lp.rightMargin = Ui.dp(c, 6);
            lp.bottomMargin = Ui.dp(c, 6);
            chip.setLayoutParams(lp);
            chip.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { switchType(t); }
            });
            typeChips[i] = chip;
            rows[i / 4].addView(chip);
        }
        View spacer = new View(c);   // 第 3 行 3 个：补隐形占位保持等宽
        spacer.setLayoutParams(new LinearLayout.LayoutParams(0, 1, 1f));
        rows[2].addView(spacer);
        typeCard.addView(rows[0]);
        typeCard.addView(rows[1]);
        typeCard.addView(rows[2]);

        // ---- 参数 ----
        LinearLayout paramCard = Forms.section(c, "参数", root);
        dyn = Ui.col(c);
        paramCard.addView(dyn);
        paintTypes();
        build(curType);
    }

    public static StepEdit build(Context c, Ops.Step work, List<String> regionIds, List<String> switchIds,
                                 List<String> tmplNames, List<String> ptsNames, String[] varSlots, Visual visual) {
        return new StepEdit(c, work, regionIds, switchIds, tmplNames, ptsNames, varSlots, visual);
    }

    private void paintTypes() {
        for (int i = 0; i < TYPE_NAMES.length; i++) {
            boolean on = i + 1 == curType;
            typeChips[i].setTextColor(on ? Ui.ACCENT : Ui.DIM);
            typeChips[i].setTypeface(typeChips[i].getTypeface(),
                    on ? android.graphics.Typeface.BOLD : android.graphics.Typeface.NORMAL);
            typeChips[i].setBackground(on
                    ? Ui.strokedRound(c, (Ui.ACCENT & 0x00FFFFFF) | 0x33000000, (Ui.ACCENT & 0x00FFFFFF) | 0x59000000, 6)
                    : Ui.strokedRound(c, 0x00000000, Ui.CARD_LINE, 6));
        }
    }

    private void switchType(int t) {
        if (t == curType) return;
        if (!read(curType)) return;    // 先回收当前参数；失败则不切换（出错字段已标红）
        // 换类型：清理不属于新类型的字段 + 给合法缺省（对齐面板加步缺省）
        if (t != Ops.T_CALC && t != Ops.T_FINDIMAGE && t != Ops.T_FINDCOLOR) work.expr = "";
        if (t != Ops.T_CALC && t != Ops.T_FINDIMAGE && t != Ops.T_FINDCOLOR
                && t != Ops.T_COND_REGION && t != Ops.T_COND_TOGGLE) work.ref = "";
        if (t == Ops.T_CALC && curType != Ops.T_CALC) {
            work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
        }
        if (t == Ops.T_FINDIMAGE && curType != Ops.T_FINDIMAGE) {
            work.a1 = 8; work.a2 = 0; work.ms = 0;
        }
        if (t == Ops.T_FINDCOLOR && curType != Ops.T_FINDCOLOR) {
            work.a1 = 0; work.a2 = 8; work.ms = 0;
        }
        if ((t == Ops.T_COND_REGION || t == Ops.T_COND_TOGGLE || t == Ops.T_FINDIMAGE || t == Ops.T_FINDCOLOR)
                && (work.a3 < 0 || work.a3 > 3 || work.a4 < 0 || work.a4 > 3)) {
            work.a3 = Ops.C_ABORT; work.a4 = Ops.C_CONT;
        }
        curType = t;
        work.type = t;
        build(t);
        paintTypes();
    }

    // ---------------- 动态字段 ----------------

    private List<String> customVarNames() {
        List<String> l = new ArrayList<String>();
        for (int i = 0; i < Ops.VAR_N; i++) if (varSlots[i] != null) l.add(varSlots[i]);
        return l;
    }

    private List<Integer> customVarCodes() {
        List<Integer> l = new ArrayList<Integer>();
        for (int i = 0; i < Ops.VAR_N; i++) if (varSlots[i] != null) l.add(Ops.customCode(i));
        return l;
    }

    private Forms.Num num(String label, int val, boolean var) {
        return num(dyn, label, val, var);
    }

    private Forms.Num num(LinearLayout host, String label, int val, boolean var) {
        Forms.Num n = new Forms.Num(c, label, val);
        if (var) n.enableVar(customVarNames(), customVarCodes());
        host.addView(n.row);
        return n;
    }

    private void build(int type) {
        dyn.removeAllViews();
        nA1 = nA2 = nA3 = nA4 = nMs = nJ1 = nJ2 = null;
        cR = cG = cB = cTol = null;
        refT = null; refC = null; regC = null; regT = null;
        calcName = null; exprE = null; calcErr = null;
        tierA4 = tierA3 = null; colorMode = null;
        j1Wrap = j2Wrap = null; singleWrap = multiWrap = null;
        chkRow = null; colorPrev = null;
        refLabel = "区域 id";
        switch (type) {
            case Ops.T_TAP:
                nA1 = num("x", work.a1, true);
                nA2 = num("y", work.a2, true);
                nMs = num("按住时长 ms", work.ms, true);
                pickXY(nA1, nA2);
                dyn.addView(varHint());
                break;
            case Ops.T_SWIPE:
                nA1 = num("起点 x", work.a1, true);
                nA2 = num("起点 y", work.a2, true);
                nA3 = num("终点 x", work.a3, true);
                nA4 = num("终点 y", work.a4, true);
                nMs = num("时长 ms（≥1）", work.ms, true);
                nJ1 = num("按下后停 ms（0 = 不停）", work.j1, true);
                nJ2 = num("弹起前停 ms（0 = 不停）", work.j2, true);
                pickXY(nA1, nA2);
                pickXY(nA3, nA4);
                dyn.addView(varHint());
                break;
            case Ops.T_WAIT:
                nMs = num("等待 ms", work.ms, true);
                dyn.addView(varHint());
                break;
            case Ops.T_DOWN:
                nA1 = num("x", work.a1, true);
                nA2 = num("y", work.a2, true);
                pickXY(nA1, nA2);
                dyn.addView(varHint());
                break;
            case Ops.T_UP:
                dyn.addView(Ui.tv(c, "（无参数）", 13, Ui.DIM, false));
                break;
            case Ops.T_COND_REGION:
                nA1 = num("判定点 x", work.a1, true);
                nA2 = num("判定点 y", work.a2, true);
                pickXY(nA1, nA2);
                dyn.addView(varHint());
                addRefRow(false);
                addTierPair();
                break;
            case Ops.T_COND_TOGGLE:
                addRefRow(true);
                addTierPair();
                break;
            case Ops.T_JUMP:
                nA1 = num("目标步（0 = 结束）", work.a1, false);
                break;
            case Ops.T_CALC:
                addCalcEditor();
                break;
            case Ops.T_FINDIMAGE:
                addNamePick(dyn, "模板（.tmpl，必选）", tmplNames, "模板名", 1);
                addRegionPick();
                nA1 = num("阈值（0..255）", work.a1, false);
                chkRow = Forms.switchRow(c, "颜色校验", "灰度命中后比对命中区域平均色（旧模板跳过）", work.a2 == 1, null);
                dyn.addView(chkRow.row);
                nMs = num("持续查找超时 ms（0 = 单次）", work.ms, false);
                addTierPair();
                break;
            case Ops.T_FINDCOLOR:
                addColorEditor();
                break;
            default:
                break;
        }
    }

    /** 给 x/y 挂「取点」：一次取点同时回填 x、y（visual 可空时跳过）。 */
    private void pickXY(Forms.Num x, Forms.Num y) {
        if (visual == null || x == null) return;
        final Forms.Num fx = x;
        final Forms.Num fy = y;
        fx.enablePick("取点 x,y", new Runnable() {
            public void run() { visual.pickPointXY(fx, fy); }
        });
    }

    /** 切换素材 chips 时刷新预览缩略图（找图行）。 */
    private void refreshRefThumb() {
        if (refThumb == null || refC == null) return;
        loadThumb(refThumb, refC.selected());
    }

    private void loadThumb(final android.widget.ImageView th, final String name) {
        if (th == null || name == null || name.isEmpty()) return;
        Ui.bg(new Runnable() {
            public void run() {
                final android.graphics.Bitmap bmp = Assets.thumbTmpl(name, Ui.dp(c, 96));
                Ui.MAIN.post(new Runnable() {
                    public void run() { if (bmp != null) th.setImageBitmap(bmp); }
                });
            }
        }, null);
    }

    private TextView varHint() {
        TextView t = Ui.tv(c, "数字可用「变量」按钮填（-1..-5 触发、-6/-7 fx/fy、自定义变量），也可直接输入负数",
                11, Ui.DIM, false);
        t.setPadding(0, Ui.dp(c, 2), 0, Ui.dp(c, 2));
        return t;
    }

    /** 计算步编辑器：变量名 + 表达式 + 插入 chips（变量 / 函数 / 运算符）。 */
    private void addCalcEditor() {
        String initName = work.ref;
        if (initName == null || initName.isEmpty()) initName = autoVarName();
        calcName = new Forms.Text(c, "变量名（[A-Za-z_][A-Za-z0-9_]*、≤15）", initName, "如 dx");
        dyn.addView(calcName.row);
        dyn.addView(Ui.label(c, "表达式（≤63；tdx/tdy 按下 · tux/tuy 弹起 · tms 时长 · fx/fy 命中坐标）"));
        exprE = new EditText(c);
        exprE.setHint("如 atan2(tuy-tdy, tux-tdx)");
        exprE.setHintTextColor(Ui.FAINT);
        exprE.setTextColor(Ui.TEXT);
        exprE.setTextSize(15);
        exprE.setTypeface(Ui.monoFont(c));
        exprE.setSingleLine(true);
        exprE.setFilters(new InputFilter[]{ new InputFilter.LengthFilter(Ops.EXPR_MAX) });
        exprE.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
        exprE.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
        exprE.setText(work.expr);
        dyn.addView(exprE);
        calcErr = Ui.tv(c, "", 11, Ui.WARN, false);
        calcErr.setVisibility(View.GONE);
        calcErr.setPadding(Ui.dp(c, 2), Ui.dp(c, 2), 0, 0);
        dyn.addView(calcErr);
        dyn.addView(Ui.label(c, "快捷公式"));
        dyn.addView(formulaStrip());
        dyn.addView(Ui.label(c, "插入变量"));
        List<String> vars = new ArrayList<String>();
        vars.add("tdx"); vars.add("tdy"); vars.add("tux"); vars.add("tuy");
        vars.add("tms"); vars.add("fx"); vars.add("fy");
        for (String vn : varSlots) if (vn != null && !vars.contains(vn)) vars.add(vn);
        dyn.addView(chipStrip(vars));
        dyn.addView(Ui.label(c, "插入函数 / 运算符"));
        List<String> fn = new ArrayList<String>();
        fn.add("atan2("); fn.add("sin("); fn.add("cos("); fn.add("abs(");
        fn.add("min("); fn.add("max("); fn.add("sqrt(");
        fn.add("+"); fn.add("-"); fn.add("*"); fn.add("/"); fn.add("("); fn.add(")"); fn.add(",");
        dyn.addView(chipStrip(fn));
    }

    /** 快捷公式（v9.3，用户口径「常用的快捷公式快速插入」）：整条表达式一点即用。
     *  插入规则：空串 / 完整表达式 = 整条替换（便于连点换公式）；以运算符、逗号、开括号结尾（正在拼装）
     *  或存在选区 = 按光标插入（如「2*」后点公式 = 2*公式）。均通过 Expr 校验语法、≤63 字符。 */
    private static final String[][] CALC_FORMULAS = {
            {"滑动角度", "atan2(tuy-tdy,tux-tdx)"},
            {"滑动距离", "sqrt((tux-tdx)*(tux-tdx)+(tuy-tdy)*(tuy-tdy))"},
            {"水平位移", "tux-tdx"},
            {"垂直位移", "tuy-tdy"},
            {"中点x", "(tdx+tux)/2"},
            {"中点y", "(tdy+tuy)/2"},
            {"滑动速度", "sqrt((tux-tdx)*(tux-tdx)+(tuy-tdy)*(tuy-tdy))/tms"},
    };

    private HorizontalScrollView formulaStrip() {
        HorizontalScrollView sc = new HorizontalScrollView(c);
        sc.setHorizontalScrollBarEnabled(false);
        LinearLayout strip = Ui.row(c);
        strip.setPadding(0, Ui.dp(c, 2), Ui.dp(c, 6), Ui.dp(c, 2));
        for (final String[] fx : CALC_FORMULAS) {
            TextView t = Ui.tv(c, fx[0], 12.5f, Ui.TEXT, false);
            t.setGravity(Gravity.CENTER);
            t.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
            t.setBackground(Ui.pressableStroked(c, 0xFF1D2126, Ui.CARD_LINE, 0xFF2A3138, 6));
            t.setClickable(true);
            t.setFocusable(true);
            t.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { insertFormula(fx[1]); }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = Ui.dp(c, 6);
            t.setLayoutParams(lp);
            strip.addView(t);
        }
        sc.addView(strip);
        return sc;
    }

    private void insertFormula(String f) {
        if (exprE == null) return;
        String tr = exprE.getText().toString().trim();
        boolean sel = exprE.getSelectionStart() != exprE.getSelectionEnd();
        boolean composing = tr.length() > 0 && "+-*/(,".indexOf(tr.charAt(tr.length() - 1)) >= 0;
        if (!sel && !composing) {
            exprE.setText(f);
            exprE.setSelection(f.length());
        } else {
            insertAtCursor(f);
        }
    }

    /** 找色编辑器：模式（单点 = 颜色+容差 / 多点 = 点集）+ 区域 + 持续 + 档位。 */
    private void addColorEditor() {
        dyn.addView(Ui.label(c, "模式"));
        colorMode = new Forms.Seg(c, new String[]{"单点（颜色+容差）", "多点（点集）"}, work.a1 == 1 ? 1 : 0,
                new Runnable() {
                    public void run() { paintColorMode(); }
                });
        dyn.addView(colorMode.row);

        singleWrap = Ui.col(c);
        int color = (work.a2 >>> 8) & 0xFFFFFF;
        int tol = work.a2 & 0xFF;
        cR = num(singleWrap, "R（0..255）", (color >> 16) & 0xFF, false);
        cG = num(singleWrap, "G（0..255）", (color >> 8) & 0xFF, false);
        cB = num(singleWrap, "B（0..255）", color & 0xFF, false);
        cTol = num(singleWrap, "容差（0..255）", tol, false);
        colorPrev = new View(c);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Ui.dp(c, 10));
        plp.topMargin = Ui.dp(c, 2);
        colorPrev.setLayoutParams(plp);
        colorPrev.setBackground(Ui.strokedRound(c, 0xFF000000 | color, Ui.CARD_LINE, 4));
        singleWrap.addView(colorPrev);
        if (visual != null) {
            TextView pickB = Ui.tv(c, "吸色", 11, Ui.ACCENT, false);
            pickB.setGravity(Gravity.CENTER);
            pickB.setBackground(Ui.pressable(c, (Ui.ACCENT & 0x00FFFFFF) | 0x33000000,
                    (Ui.ACCENT & 0x00FFFFFF) | 0x55000000, 6));
            pickB.setPadding(Ui.dp(c, 12), Ui.dp(c, 8), Ui.dp(c, 12), Ui.dp(c, 8));
            pickB.setClickable(true);
            pickB.setFocusable(true);
            pickB.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    visual.pickColor(new ColorSink() {
                        public void onColor(int rgb) {
                            cR.setValue((rgb >> 16) & 0xFF);
                            cG.setValue((rgb >> 8) & 0xFF);
                            cB.setValue(rgb & 0xFF);
                            paintColorPrev();
                        }
                    });
                }
            });
            singleWrap.addView(pickB);
        }
        TextWatcher tw = new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int st, int cnt, int af) { }
            public void onTextChanged(CharSequence s, int st, int b, int cnt) { }
            public void afterTextChanged(Editable s) { paintColorPrev(); }
        };
        cR.field.addTextChangedListener(tw);
        cG.field.addTextChangedListener(tw);
        cB.field.addTextChangedListener(tw);
        dyn.addView(singleWrap);

        multiWrap = Ui.col(c);
        addNamePick(multiWrap, "点集（.pts，多点必选）", ptsNames, "点集名", 2);
        dyn.addView(multiWrap);

        addRegionPick();
        nMs = num("持续查找超时 ms（0 = 单次）", work.ms, false);
        addTierPair();
        paintColorMode();
    }

    private void paintColorMode() {
        boolean multi = colorMode != null && colorMode.selected() == 1;
        if (singleWrap != null) singleWrap.setVisibility(multi ? View.GONE : View.VISIBLE);
        if (multiWrap != null) multiWrap.setVisibility(multi ? View.VISIBLE : View.GONE);
    }

    private void paintColorPrev() {
        if (colorPrev == null) return;
        int r = clamp255(peek(cR)), g = clamp255(peek(cG)), b = clamp255(peek(cB));
        colorPrev.setBackground(Ui.strokedRound(c, 0xFF000000 | (r << 16) | (g << 8) | b, Ui.CARD_LINE, 4));
    }

    private static int peek(Forms.Num n) {
        try { return Integer.parseInt(n.field.getText().toString().trim()); }
        catch (Exception e) { return 0; }
    }

    private static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    private String autoVarName() {
        for (int i = 1; i <= 999; i++) {
            String cand = "v" + i;
            boolean used = false;
            for (String s : varSlots) if (cand.equals(s)) { used = true; break; }
            if (!used) return cand;
        }
        return "v";
    }

    /** 当前选择的名字（chips 或手输）。 */
    private String currentRef() {
        if (refC != null) return refC.selected();
        if (refT != null) return refT.value();
        return "";
    }

    /** 模板 / 点集名选择（列表为空时退化为手工输入）；kind 1=模板 2=点集（带素材入口）。 */
    private void addNamePick(LinearLayout host, String label, List<String> names, String what, int kind) {
        host.addView(Ui.label(c, label));
        refLabel = what;
        refThumb = null;
        String sel = null;
        if (names != null && !names.isEmpty()) {
            List<String> ids = new ArrayList<String>(names);
            if (work.ref != null && !work.ref.isEmpty() && !ids.contains(work.ref)) ids.add(0, work.ref);
            sel = (work.ref != null && ids.contains(work.ref)) ? work.ref : ids.get(0);
            refC = new Forms.Chips(c, ids, sel, new Runnable() {
                public void run() { refreshRefThumb(); }
            });
            host.addView(refC.view);
        } else {
            refT = new Forms.Text(c, "（无可选项时手工输入）", work.ref == null ? "" : work.ref, "");
            host.addView(refT.row);
        }
        if (visual == null) return;
        LinearLayout acts = Ui.row(c);
        acts.setPadding(0, Ui.dp(c, 4), 0, Ui.dp(c, 2));
        if (kind == 1) {
            TextView bNew = Ui.btnSmall(c, "＋ 新建模板");
            bNew.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { visual.newTmpl(refC); }
            });
            TextView bEdit = Ui.btnSmall(c, "编辑");
            bEdit.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { visual.editTmpl(currentRef()); }
            });
            TextView bTest = Ui.btnSmall(c, "试一下");
            bTest.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    int th = 8;
                    try {
                        if (nA1 != null) {
                            int v2 = nA1.value();
                            if (v2 >= 0) th = v2;
                        }
                    } catch (Exception ig) { }
                    boolean cc = chkRow != null && chkRow.sw.isChecked();
                    String reg = regC != null ? regC.selected() : "";
                    if ("（全屏）".equals(reg)) reg = "";
                    visual.testTmpl(currentRef(), th, cc, reg);
                }
            });
            acts.addView(bNew);
            acts.addView(bEdit);
            acts.addView(bTest);
            host.addView(acts);
            if (refC != null && sel != null) {
                refThumb = new android.widget.ImageView(c);
                LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(Ui.dp(c, 96), Ui.dp(c, 96));
                tlp.topMargin = Ui.dp(c, 4);
                refThumb.setLayoutParams(tlp);
                refThumb.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
                refThumb.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
                host.addView(refThumb);
                loadThumb(refThumb, sel);
            }
        } else {
            TextView bNew = Ui.btnSmall(c, "＋ 新建点集");
            bNew.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { visual.newPts(refC); }
            });
            TextView bEdit = Ui.btnSmall(c, "编辑");
            bEdit.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { visual.editPts(currentRef()); }
            });
            TextView bTest = Ui.btnSmall(c, "试一下");
            bTest.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    String reg = regC != null ? regC.selected() : "";
                    if ("（全屏）".equals(reg)) reg = "";
                    visual.testPts(currentRef(), reg);
                }
            });
            acts.addView(bNew);
            acts.addView(bEdit);
            acts.addView(bTest);
            host.addView(acts);
        }
    }

    /** 视觉步「区域限定」（空 = 全屏；复用 expr 列）。 */
    private void addRegionPick() {
        List<String> base = new ArrayList<String>();
        base.add("（全屏）");
        if (regionIds != null) base.addAll(regionIds);
        String cur = work.expr == null ? "" : work.expr;
        if (!cur.isEmpty() && !base.contains(cur)) base.add(0, cur);   // 旧值保留展示
        String sel = cur.isEmpty() ? "（全屏）" : cur;
        dyn.addView(Ui.label(c, "区域限定（全屏 = 不限）"));
        regC = new Forms.Chips(c, base, sel, null);
        dyn.addView(regC.view);
    }

    /** 区域 id 选择行：区域判断 = 任意区域；开关判断 = 仅开关型。 */
    private void addRefRow(boolean needSwitch) {
        List<String> base = needSwitch ? switchIds : regionIds;
        refLabel = needSwitch ? "开关型区域 id" : "区域 id";
        if (base != null && !base.isEmpty()) {
            dyn.addView(Ui.label(c, needSwitch ? "开关型区域 id（仅开关型）" : "区域 id"));
            List<String> ids = new ArrayList<String>(base);
            if (work.ref != null && !work.ref.isEmpty() && !ids.contains(work.ref)) {
                ids.add(0, work.ref);   // 旧值 / 已失效：保留展示，保存时校验
            }
            String sel = (work.ref != null && ids.contains(work.ref)) ? work.ref : ids.get(0);
            refC = new Forms.Chips(c, ids, sel, null);
            dyn.addView(refC.view);
        } else {
            refT = new Forms.Text(c,
                    needSwitch ? "开关型区域 id（暂无开关型区域，先去区域编辑打开「开关型」）"
                               : "区域 id（如 c1）",
                    work.ref == null ? "" : work.ref, "");
            dyn.addView(refT.row);
        }
    }

    private void addTierPair() {
        final int t4 = (work.a4 >= 0 && work.a4 <= 3) ? work.a4 : 0;
        final int t3 = (work.a3 >= 0 && work.a3 <= 3) ? work.a3 : 0;

        dyn.addView(Ui.label(c, "成立侧（命中 / 条件为真）"));
        j1Wrap = Ui.col(c);
        j1Wrap.setVisibility(t4 == Ops.C_JUMP ? View.VISIBLE : View.GONE);
        nJ1 = num(j1Wrap, "跳转目标步（0 = 结束）", work.j1, false);
        tierA4 = new Forms.Seg(c, new String[]{"中止", "跳步", "继续", "跳转"}, t4, new Runnable() {
            public void run() {
                j1Wrap.setVisibility(tierA4.selected() == Ops.C_JUMP ? View.VISIBLE : View.GONE);
            }
        });
        dyn.addView(tierA4.row);
        dyn.addView(j1Wrap);

        dyn.addView(Ui.label(c, "不成立侧（未命中 / 条件为假）"));
        j2Wrap = Ui.col(c);
        j2Wrap.setVisibility(t3 == Ops.C_JUMP ? View.VISIBLE : View.GONE);
        nJ2 = num(j2Wrap, "跳转目标步（0 = 结束）", work.j2, false);
        tierA3 = new Forms.Seg(c, new String[]{"中止", "跳步", "继续", "跳转"}, t3, new Runnable() {
            public void run() {
                j2Wrap.setVisibility(tierA3.selected() == Ops.C_JUMP ? View.VISIBLE : View.GONE);
            }
        });
        dyn.addView(tierA3.row);
        dyn.addView(j2Wrap);
    }

    private HorizontalScrollView chipStrip(List<String> items) {
        HorizontalScrollView sc = new HorizontalScrollView(c);
        sc.setHorizontalScrollBarEnabled(false);
        LinearLayout strip = Ui.row(c);
        strip.setPadding(0, Ui.dp(c, 2), Ui.dp(c, 6), Ui.dp(c, 2));
        for (String it : items) {
            final String tok = it;
            TextView t = Ui.tv(c, it, 12.5f, Ui.ACCENT, false);
            t.setGravity(Gravity.CENTER);
            t.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
            t.setBackground(Ui.pressableStroked(c, 0x00000000, (Ui.ACCENT & 0x00FFFFFF) | 0x59000000, 0xFF2A3138, 6));
            t.setClickable(true);
            t.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { insertAtCursor(tok); }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = Ui.dp(c, 6);
            t.setLayoutParams(lp);
            strip.addView(t);
        }
        sc.addView(strip);
        return sc;
    }

    private void insertAtCursor(String text) {
        if (exprE == null) return;
        String cur = exprE.getText().toString();
        int st = Math.max(0, Math.min(exprE.getSelectionStart(), cur.length()));
        int en = Math.max(st, Math.min(exprE.getSelectionEnd(), cur.length()));
        String nv = cur.substring(0, st) + text + cur.substring(en);
        if (nv.length() > Ops.EXPR_MAX) nv = nv.substring(0, Ops.EXPR_MAX);
        exprE.setText(nv);
        exprE.setSelection(Math.min(nv.length(), st + text.length()));
    }

    // ---------------- 回收 ----------------

    private int iv(Forms.Num n) {
        try {
            int v = Integer.parseInt(n.field.getText().toString().trim());
            n.clearError();
            return v;
        } catch (Exception e) {
            n.setError("必须是整数（可为负）");
            throw new Bad();
        }
    }

    /** 字面值在 [lo,hi]；allowVar = 同时接受负数编码 -25..-1。 */
    private int ivField(Forms.Num n, int lo, int hi, boolean allowVar, String what) {
        int v = iv(n);
        boolean ok = (v >= lo && v <= hi) || (allowVar && v >= Ops.V_CUSTOM15 && v <= Ops.V_TDX);
        if (!ok) {
            n.setError(what + "需在 " + lo + ".." + hi + (allowVar ? " 或变量" : ""));
            throw new Bad();
        }
        n.clearError();
        return v;
    }

    private int ivCoord(Forms.Num n, boolean x, String what) {
        int v = iv(n);
        int max = x ? (Regions.lastFrameW > 0 ? Regions.lastFrameW - 1 : -1)
                    : (Regions.lastFrameH > 0 ? Regions.lastFrameH - 1 : -1);
        boolean ok = (v >= 0 && (max < 0 || v <= max)) || (v >= Ops.V_CUSTOM15 && v <= Ops.V_TDX);
        if (!ok) {
            n.setError(what + "越界（0.." + (max < 0 ? "?" : String.valueOf(max)) + " 或变量）");
            throw new Bad();
        }
        n.clearError();
        return v;
    }

    private String readRef() {
        String r;
        if (refC != null) r = refC.selected();
        else if (refT != null) r = refT.value();
        else r = work.ref;
        if (r == null || r.isEmpty()) {
            if (refT != null) refT.setError("必填：" + refLabel);
            throw new Bad();
        }
        if (!Regions.idOk(r)) {
            if (refT != null) refT.setError(refLabel + "非法（[A-Za-z0-9_-]、1..15）");
            throw new Bad();
        }
        if (refT != null) refT.clearError();
        return r;
    }

    private String readRegion() {
        if (regC != null) {
            String s = regC.selected();
            return "（全屏）".equals(s) ? "" : s;
        }
        if (regT != null) {
            String r = regT.value();
            if (!r.isEmpty() && !Regions.idOk(r)) { regT.setError("区域 id 非法"); throw new Bad(); }
            regT.clearError();
            return r;
        }
        return work.expr == null ? "" : work.expr;
    }

    private void setExprError(String s) {
        if (calcErr != null) { calcErr.setText(s); calcErr.setVisibility(View.VISIBLE); }
        if (exprE != null) exprE.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.WARN, 6));
    }

    private void clearExprError() {
        if (calcErr != null) calcErr.setVisibility(View.GONE);
        if (exprE != null) exprE.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
    }

    /** 表达式可引用的名字：操作现有自定义变量（排除本步旧名）+ 本步新名。 */
    private List<String> exprNames(String newName) {
        List<String> l = new ArrayList<String>();
        String old = work.ref == null ? "" : work.ref;
        for (String s : varSlots) if (s != null && !s.equals(old)) l.add(s);
        if (newName != null && !newName.isEmpty() && !l.contains(newName)) l.add(newName);
        return l;
    }

    /** 读当前类型可见字段 → work；出错标红并返回 false。 */
    private boolean read(int type) {
        try {
            switch (type) {
                case Ops.T_TAP:
                    work.a1 = ivCoord(nA1, true, "x");
                    work.a2 = ivCoord(nA2, false, "y");
                    work.ms = ivField(nMs, 0, 60000, true, "时长");
                    break;
                case Ops.T_SWIPE:
                    work.a1 = ivCoord(nA1, true, "起点 x");
                    work.a2 = ivCoord(nA2, false, "起点 y");
                    work.a3 = ivCoord(nA3, true, "终点 x");
                    work.a4 = ivCoord(nA4, false, "终点 y");
                    work.ms = ivField(nMs, 1, 60000, true, "时长");
                    work.j1 = ivField(nJ1, 0, 60000, true, "按下后停");
                    work.j2 = ivField(nJ2, 0, 60000, true, "弹起前停");
                    break;
                case Ops.T_WAIT:
                    work.ms = ivField(nMs, 0, 600000, true, "等待时长");
                    break;
                case Ops.T_DOWN:
                    work.a1 = ivCoord(nA1, true, "x");
                    work.a2 = ivCoord(nA2, false, "y");
                    break;
                case Ops.T_UP:
                    break;
                case Ops.T_COND_REGION:
                    work.a1 = ivCoord(nA1, true, "判定点 x");
                    work.a2 = ivCoord(nA2, false, "判定点 y");
                    work.ref = readRef();
                    work.a4 = tierA4.selected();
                    work.a3 = tierA3.selected();
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                case Ops.T_COND_TOGGLE:
                    work.ref = readRef();
                    work.a4 = tierA4.selected();
                    work.a3 = tierA3.selected();
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                case Ops.T_JUMP:
                    work.a1 = iv(nA1);
                    break;
                case Ops.T_CALC: {
                    String nm = calcName.value();
                    if (!Ops.varNameOk(nm)) {
                        calcName.setError("变量名非法（[A-Za-z_][A-Za-z0-9_]*、≤15）");
                        throw new Bad();
                    }
                    calcName.clearError();
                    int idx = -1;
                    for (int i = 0; i < Ops.VAR_N; i++) if (nm.equals(varSlots[i])) { idx = i; break; }
                    if (idx < 0) {                       // 新名字：本步原槽可用就保留，否则找空槽
                        if (work.a1 >= 0 && work.a1 < Ops.VAR_N
                                && (varSlots[work.a1] == null || varSlots[work.a1].equals(work.ref))) {
                            idx = work.a1;
                        } else {
                            for (int i = 0; i < Ops.VAR_N; i++) if (varSlots[i] == null) { idx = i; break; }
                        }
                        if (idx < 0) { calcName.setError("变量已满（16 个）"); throw new Bad(); }
                    }
                    String ex = exprE.getText().toString().trim();
                    if (ex.isEmpty()) { setExprError("表达式不能为空"); throw new Bad(); }
                    String why = Expr.check(ex, exprNames(nm));
                    if (why != null) { setExprError("表达式错：" + why); throw new Bad(); }
                    clearExprError();
                    work.a1 = idx;
                    work.ref = nm;
                    work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
                    work.expr = ex;
                    break;
                }
                case Ops.T_FINDIMAGE:
                    work.ref = readRef();
                    work.expr = readRegion();
                    work.a1 = ivField(nA1, 0, 255, false, "阈值");
                    work.a2 = (chkRow != null && chkRow.sw.isChecked()) ? 1 : 0;
                    work.ms = ivField(nMs, 0, 60000, false, "超时");
                    work.a4 = tierA4.selected();
                    work.a3 = tierA3.selected();
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                case Ops.T_FINDCOLOR: {
                    boolean multi = colorMode.selected() == 1;
                    work.a1 = multi ? 1 : 0;
                    if (multi) {
                        work.ref = readRef();
                        work.a2 = 0;
                    } else {
                        work.ref = "";
                        int r = ivField(cR, 0, 255, false, "R");
                        int g = ivField(cG, 0, 255, false, "G");
                        int b = ivField(cB, 0, 255, false, "B");
                        int tol = ivField(cTol, 0, 255, false, "容差");
                        work.a2 = ((r << 16) | (g << 8) | b) << 8 | tol;
                    }
                    work.expr = readRegion();
                    work.ms = ivField(nMs, 0, 60000, false, "超时");
                    work.a4 = tierA4.selected();
                    work.a3 = tierA3.selected();
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                }
                default:
                    break;
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /** 回收并校验：成功返回 null（work 已写回最新参数）；失败返回提示（出错字段已标红）。 */
    public String collect() {
        if (!read(curType)) return "有字段未通过校验（已标红）";
        work.type = curType;
        return null;
    }
}
