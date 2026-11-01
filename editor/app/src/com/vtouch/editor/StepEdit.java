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
        void newTmpl(AssetTarget target);
        void editTmpl(String name);
        void testTmpl(String name, int thresh, boolean colorCheck, String region);
        void newPts(AssetTarget target);
        void editPts(String name);
        void testPts(String name, String region);
        void pickPointXY(Forms.Num x, Forms.Num y);
        void pickColor(ColorSink sink);
    }

    /** 吸色回填接收端。 */
    public interface ColorSink { void onColor(int rgb); }
    public interface AssetTarget { void onSaved(String name); }

    private static final String[] TYPE_NAMES =
            {"点按", "滑动", "等待", "按下", "弹起", "区域判断", "开关判断", "跳转", "计算", "找图", "找色",
             "循环", "条件块", "跳出循环", "继续循环", "终止", "提示", "框区域", "框矩形"};
    private static final int[] TYPE_IDS =
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 17, 18, 20, 21, 22, 23};

    /** 名称 → 类型号（供类型弹层用；与 TYPE_NAMES/TYPE_IDS 同序）。 */
    public static int typeIdByName(String name) {
        for (int i = 0; i < TYPE_NAMES.length; i++) if (TYPE_NAMES[i].equals(name)) return TYPE_IDS[i];
        return Ops.T_TAP;
    }

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

    private TextView typeValue;
    private final LinearLayout dyn;
    private android.widget.ImageView refThumb;

    private Forms.Num nA1, nA2, nA3, nA4, nMs, nJ1, nJ2;
    private Forms.Num cR, cG, cB, cTol;
    private Forms.Text refT, calcName;
    private Forms.Chips refC, regC;
    private Forms.Chips hlChips;                          // v6E：框区域 区域选择
    private Forms.Text regT;
    private Forms.Seg tierA4, tierA3, colorMode;
    private LinearLayout j1Wrap, j2Wrap, singleWrap, multiWrap;
    // v12 控制流（循环 / 条件块）
    private Forms.Seg loopMode;
    private LinearLayout loopBody;
    // v6B：子条件 / 终止 / 跳出层数（v6C：判定与子条件 = CondBox 实例，同页可并存）
    private java.util.List<Ops.Step> clauseRows;      // 块头现有子条件（构造入参，副本）
    private java.util.List<CondBox> clauseBoxes;      // 子条件编辑器列表（增删在页内完成；保存走组替换）
    private LinearLayout clauseBox;                   // 子条件区容器（重建用）
    private TextView clauseAddBtn;                    // v32：[＋ 加子条件] 键（测试直驱真实点按）
    private Forms.Seg joinMode, stopMode;             // 19 连接词 / 20 终止模式
    private Forms.Num nDepth;                         // 17/18 层数
    private CondBox headerBox;                        // 本页主判定编辑器（14 / 12 模式3 / 19 各持一箱）
    private EditText exprE;
    private TextView calcErr;
    private EditText tvE;                             // v6D：提示模板输入框
    private TextView tvPrev;                          // v6D：提示实时预览 / 错误行
    private Forms.SwitchRow chkRow;
    private View colorPrev;
    private String refLabel = "区域 id";

    private static final class Bad extends RuntimeException {
        Bad() { super("bad-field"); }
    }

    private StepEdit(Context c, Ops.Step work, List<String> regionIds, List<String> switchIds,
                     List<String> tmplNames, List<String> ptsNames, String[] varSlots, Visual visual,
                     List<Ops.Step> clauseRows) {
        this.c = c;
        this.work = work;
        this.regionIds = regionIds;
        this.switchIds = switchIds;
        this.tmplNames = tmplNames;
        this.ptsNames = ptsNames;
        this.varSlots = varSlots != null ? varSlots : new String[Ops.VAR_N];
        this.visual = visual;
        this.clauseRows = clauseRows;
        if (clauseRows != null) {                     // v6C：子条件 = CondBox 列表（无 UI，build 在 paintClauses 里做）
            this.clauseBoxes = new java.util.ArrayList<CondBox>();
            for (Ops.Step s : clauseRows) {
                CondBox b = new CondBox(2, s.copy());
                b.load();
                this.clauseBoxes.add(b);
            }
        }
        curType = work.type;
        root = Ui.col(c);
        root.setPadding(Ui.dp(c, 12), Ui.dp(c, 0), Ui.dp(c, 12), Ui.dp(c, 12));

        // ---- 类型（v12.1 一行式：点开底部弹层更换——不占版面） ----
        LinearLayout typeCard = Forms.section(c, "步骤类型", root);
        LinearLayout trow = Ui.row(c);
        trow.setGravity(Gravity.CENTER_VERTICAL);
        trow.setBackground(Ui.pressable(c, 0x00000000, 0x40FFFFFF, 8));
        trow.setPadding(Ui.dp(c, 12), Ui.dp(c, 12), Ui.dp(c, 12), Ui.dp(c, 12));
        trow.setClickable(true);
        typeValue = Ui.tv(c, Ops.typeName(curType), 15, Ui.ACCENT, true);
        trow.addView(typeValue, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        trow.addView(Ui.tv(c, "▾ 点选更换", 12, Ui.DIM, false));
        trow.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Ui.Dlg d = Forms.typeSheet(c, new Forms.TypePick() {
                    public void onPick(int t) { switchType(t); }
                });
                Ui.showSheet(d);
            }
        });
        typeCard.addView(trow);

        // ---- 参数 ----
        LinearLayout paramCard = Forms.section(c, "参数", root);
        dyn = Ui.col(c);
        paramCard.addView(dyn);
        paintTypes();
        build(curType);
    }

    public static StepEdit build(Context c, Ops.Step work, List<String> regionIds, List<String> switchIds,
                                 List<String> tmplNames, List<String> ptsNames, String[] varSlots, Visual visual,
                                 List<Ops.Step> clauseRows) {
        return new StepEdit(c, work, regionIds, switchIds, tmplNames, ptsNames, varSlots, visual, clauseRows);
    }

    private void paintTypes() {
        if (typeValue != null) typeValue.setText(Ops.typeName(curType));
    }

    private void switchType(int t) {
        if (t == curType) return;
        if (!read(curType)) return;    // 先回收当前参数；失败则不切换（出错字段已标红）
        // v6B：带子条件的块头不得换成别的类型（先删子条件，避免链断裂）
        if (clauseBoxes != null && !clauseBoxes.isEmpty() && t != Ops.T_IF && t != Ops.T_LOOP) {
            Ui.toast("该块头带 " + clauseBoxes.size() + " 条子条件：先删除子条件再换类型");
            return;
        }
        // 换类型：清理不属于新类型的字段 + 给合法缺省（对齐面板加步缺省）
        if (t != Ops.T_CALC && t != Ops.T_FINDIMAGE && t != Ops.T_FINDCOLOR
                && t != Ops.T_LOOP && t != Ops.T_IF) work.expr = "";
        if (t != Ops.T_CALC && t != Ops.T_FINDIMAGE && t != Ops.T_FINDCOLOR
                && t != Ops.T_COND_REGION && t != Ops.T_COND_TOGGLE
                && t != Ops.T_LOOP && t != Ops.T_IF) work.ref = "";
        if (t == Ops.T_CALC && curType != Ops.T_CALC) {
            work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
        }
        if (t == Ops.T_FINDIMAGE && curType != Ops.T_FINDIMAGE) {
            work.a1 = 8; work.a2 = 0; work.ms = 0;
        }
        if (t == Ops.T_FINDCOLOR && curType != Ops.T_FINDCOLOR) {
            work.a1 = 0; work.a2 = 8; work.ms = 0;
        }
        if (t == Ops.T_LOOP && curType != Ops.T_LOOP) {
            work.a1 = 0; work.a2 = 3; work.a3 = 0; work.a4 = 0;
            work.ms = 0; work.j1 = 0; work.j2 = 0; work.ref = ""; work.expr = "";
        }
        if (t == Ops.T_IF && curType != Ops.T_IF) {
            work.a1 = 0; work.a2 = 0; work.a3 = 0; work.a4 = 0;
            work.ms = 0; work.j1 = 0; work.j2 = 0; work.ref = ""; work.expr = "";
        }
        if (t == Ops.T_BREAK || t == Ops.T_CONT) {
            work.a1 = work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
            work.ref = ""; work.expr = "";
        }
        if (t == Ops.T_STOP) {
            work.a1 = work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
            work.ref = ""; work.expr = "";
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

    private void addAdvanced(LinearLayout host, final LinearLayout body, boolean expanded) {
        final TextView toggle = Ui.btnFlat(c, expanded ? "高级选项 ▴" : "高级选项 ▾", Ui.ACCENT);
        body.setVisibility(expanded ? View.VISIBLE : View.GONE);
        toggle.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                boolean show = body.getVisibility() != View.VISIBLE;
                body.setVisibility(show ? View.VISIBLE : View.GONE);
                toggle.setText(show ? "高级选项 ▴" : "高级选项 ▾");
            }
        });
        body.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int oright, int ob) {
                toggle.setText(body.getVisibility() == View.VISIBLE ? "高级选项 ▴" : "高级选项 ▾");
            }
        });
        host.addView(toggle);
        host.addView(body);
    }

    private void revealField(View field) {
        android.view.ViewParent parent = field.getParent();
        while (parent instanceof View && parent != root) {
            View v = (View) parent;
            if (v.getVisibility() == View.GONE) v.setVisibility(View.VISIBLE);
            parent = v.getParent();
        }
        field.requestFocus();
    }

    private void build(int type) {
        dyn.removeAllViews();
        nA1 = nA2 = nA3 = nA4 = nMs = nJ1 = nJ2 = null;
        cR = cG = cB = cTol = null;
        refT = null; refC = null; regC = null; regT = null; hlChips = null;
        calcName = null; exprE = null; calcErr = null;
        tvE = null; tvPrev = null;
        tierA4 = tierA3 = null; colorMode = null;
        j1Wrap = j2Wrap = null; singleWrap = multiWrap = null;
        chkRow = null; colorPrev = null;
        joinMode = null; stopMode = null; nDepth = null; clauseBox = null; headerBox = null;
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
                pickXY(nA1, nA2);
                pickXY(nA3, nA4);
                dyn.addView(varHint());
                final LinearLayout swipeExtra = Ui.col(c);
                nJ1 = num(swipeExtra, "按下后停 ms（0 = 不停）", work.j1, true);
                nJ2 = num(swipeExtra, "弹起前停 ms（0 = 不停）", work.j2, true);
                addAdvanced(dyn, swipeExtra, work.j1 != 0 || work.j2 != 0);
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
                nA1 = num("阈值（0..255）", work.a1, false);
                LinearLayout imageExtra = Ui.col(c);
                addRegionPick(imageExtra);
                chkRow = Forms.switchRow(c, "颜色校验", "灰度命中后比对命中区域平均色（旧模板跳过）", work.a2 == 1, null);
                imageExtra.addView(chkRow.row);
                nMs = num(imageExtra, "持续查找超时 ms（0 = 单次）", work.ms, false);
                addAdvanced(dyn, imageExtra, work.a2 == 1 || work.ms != 0 || (work.expr != null && !work.expr.isEmpty()));
                addTierPair();
                break;
            case Ops.T_FINDCOLOR:
                addColorEditor();
                break;
            case Ops.T_LOOP:
                addLoopEditor();
                break;
            case Ops.T_IF: {
                headerBox = new CondBox(0, work);
                headerBox.load();
                headerBox.build(dyn);
                addClauseSection(dyn);                 // v6B/v6C：子条件（同页内联编辑器）
                break;
            }
            case Ops.T_CLAUSE:
                joinMode = new Forms.Seg(c, new String[]{"且", "或"}, (work.a1 == Ops.J_OR) ? 1 : 0,
                        new Runnable() { public void run() { } });
                dyn.addView(joinMode.row);
                headerBox = new CondBox(2, work);
                headerBox.load();
                headerBox.build(dyn);
                break;
            case Ops.T_STOP:
                stopMode = new Forms.Seg(c, new String[]{"完成收工", "中止退出"}, (work.a1 == 1) ? 1 : 0,
                        new Runnable() { public void run() { } });
                dyn.addView(stopMode.row);
                dyn.addView(Ui.tv(c, "完成收工：立即结束整个操作（收尾释放 + 门控自动关照常翻回关）。\n中止退出：立即结束整个操作并记为中止（不翻门控）。", 12.5f, Ui.DIM, false));
                break;
            case Ops.T_TOAST: {
                if (work.ms <= 0) work.ms = 2000;         // v6D：默认显示 2 秒（核心 0 也按 2000）
                dyn.addView(Ui.label(c, "提示文本（{变量名} → 当前值；{{ }} → 花括号）"));
                tvE = new EditText(c);
                tvE.setHint("如：已连击 {e9} 次 · fx={fx}");
                tvE.setHintTextColor(Ui.FAINT);
                tvE.setTextColor(Ui.TEXT);
                tvE.setTextSize(14.5f);
                tvE.setSingleLine(true);
                tvE.setFilters(new InputFilter[]{ new InputFilter.LengthFilter(Ops.EXPR_MAX) });
                tvE.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
                tvE.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
                tvE.setText(work.expr == null ? "" : work.expr);
                tvE.addTextChangedListener(new TextWatcher() {
                    public void beforeTextChanged(CharSequence s, int st, int cnt, int af) { }
                    public void onTextChanged(CharSequence s, int st, int b, int cnt) { }
                    public void afterTextChanged(Editable s) { refreshToastPreview(); }
                });
                dyn.addView(tvE);
                tvPrev = Ui.tv(c, "", 11.5f, Ui.FAINT, false);
                tvPrev.setPadding(Ui.dp(c, 2), Ui.dp(c, 3), 0, 0);
                dyn.addView(tvPrev);
                dyn.addView(Ui.label(c, "插入变量"));
                List<String> tvn = new ArrayList<String>();
                tvn.add("tdx"); tvn.add("tdy"); tvn.add("tux"); tvn.add("tuy");
                tvn.add("tms"); tvn.add("fx"); tvn.add("fy");
                for (String vn : varSlots) if (vn != null && !tvn.contains(vn)) tvn.add(vn);
                dyn.addView(chipStripBraced(tvn, tvE));
                nMs = num("显示时长 ms（0 = 默认 2 秒）", work.ms, false);
                dyn.addView(Ui.tv(c, "运行到该步：屏幕顶部显示此文本并自动淡出（不阻塞，显示完立即执行下一步）。引用未写入值的变量会中止（同计算步口径）。", 12.5f, Ui.DIM, false));
                refreshToastPreview();
                break;
            }
            case Ops.T_HL: {
                List<String> rids = new ArrayList<String>();
                if (regionIds != null) rids.addAll(regionIds);
                String rcur = work.ref == null ? "" : work.ref;
                if (!rids.isEmpty()) {
                    if (!rcur.isEmpty() && !rids.contains(rcur)) rids.add(0, rcur);
                    dyn.addView(Ui.label(c, "区域 id"));
                    hlChips = new Forms.Chips(c, rids, rcur.isEmpty() ? rids.get(0) : rcur, null);
                    dyn.addView(hlChips.view);
                } else {
                    dyn.addView(Ui.tv(c, "暂无区域（先去区域编辑里新建一个再回来）", 12.5f, Ui.WARN, false));
                }
                nMs = num("显示时长 ms（0 = 默认 3 秒）", work.ms, false);
                dyn.addView(Ui.tv(c, "运行到该步：屏幕上把该区域框出来（琥珀高亮 + 区域名，自动淡出；面板「区域显隐」关着也显示）。含停用区域。", 12.5f, Ui.DIM, false));
                break;
            }
            case Ops.T_BOX:
                nA1 = num("x1（左上 x）", work.a1, true);
                nA2 = num("y1（左上 y）", work.a2, true);
                nA3 = num("x2（右下 x）", work.a3, true);
                nA4 = num("y2（右下 y）", work.a4, true);
                nMs = num("显示时长 ms（0 = 默认 3 秒）", work.ms, false);
                pickXY(nA1, nA2);
                pickXY(nA3, nA4);
                dyn.addView(varHint());
                dyn.addView(Ui.tv(c, "运行到该步：屏幕上画出这个矩形框（琥珀高亮，自动淡出）。x2/y2 填反了会自动调正；坐标可以填变量——如找图/找色的命中点 fx/fy，配合计算步先把四角算好。", 12.5f, Ui.DIM, false));
                break;
            case Ops.T_BREAK:
            case Ops.T_CONT:
                nDepth = num("层数（1 = 最内层，默认）", (work.a1 >= 1) ? work.a1 : 1, false);
                dyn.addView(Ui.tv(c, "2 = 往外跳一层……上限 = 该处实际循环嵌套层数（≤8）", 12.5f, Ui.DIM, false));
                break;
            default:
                break;
        }
    }

    /* ---------------- v12 控制流编辑器（循环 / 条件块） ---------------- */

    /** 循环编辑器：模式 Seg（次数 / 时长 / 无限 / 到条件）+ 对应值区。 */
    private void addLoopEditor() {
        loopMode = new Forms.Seg(c, new String[]{"次数", "时长", "无限", "到条件"},
                (work.a1 >= 0 && work.a1 <= 3) ? work.a1 : 0, new Runnable() {
            public void run() { paintLoop(); }
        });
        dyn.addView(loopMode.row);
        loopBody = Ui.col(c);
        dyn.addView(loopBody);
        paintLoop();
    }

    private void paintLoop() {
        loopBody.removeAllViews();
        nA2 = null;
        int mode = loopMode.selected();
        if (mode == 0) {
            nA2 = num(loopBody, "次数（1..99999）", (work.a2 >= 1 && work.a2 <= 99999) ? work.a2 : 3, false);
        } else if (mode == 1) {
            nA2 = num(loopBody, "时长 ms（1000..86400000）",
                    (work.a2 >= 1000 && work.a2 <= 86400000) ? work.a2 : 1800000, false);
        } else if (mode == 2) {
            loopBody.addView(Ui.tv(c, "无限循环：跑不停，由面板 [停止] 结束", 12.5f, Ui.DIM, false));
        } else {
            headerBox = new CondBox(1, work);
            headerBox.load();
            headerBox.build(loopBody);
            addClauseSection(loopBody);                // v6B/v6C：到条件的子条件（同页内联编辑器）
        }
        if (mode != 3 && clauseBoxes != null && !clauseBoxes.isEmpty()) {
            loopBody.addView(Ui.tv(c, "⚠ 该循环带 " + clauseBoxes.size() + " 条子条件，仅「到条件」模式生效：请切到「到条件」或先删除", 12, Ui.WARN, false));
        }
    }

    /* v6C：判定编辑器已抽成 CondBox 实例（见本文件「判定编辑器（CondBox）」节）——头子句 / 每条子条件各持一箱，同页并存、互不干扰。 */

    /* ---------------- v6B/v6C：子条件列表（块头编辑页；每条 = 完整内联编辑器，一页配完） ---------------- */

    /** 子条件区（块头编辑页内）：列表 + [＋ 加子条件]（合计 ≤ CLAUSE_MAX）；每条直接在本页配置。 */
    private void addClauseSection(LinearLayout host) {
        if (clauseBoxes == null) clauseBoxes = new java.util.ArrayList<CondBox>();
        clauseBox = Ui.col(c);
        host.addView(clauseBox);
        paintClauses();
    }

    private void paintClauses() {
        if (clauseBox == null) return;
        if (clauseBoxes != null) for (CondBox b : clauseBoxes) b.harvest();   // 重建前先回收（保打字中的值）
        clauseBox.removeAllViews();
        clauseBox.addView(Ui.tv(c, "── 子条件（同代码的 与 / 或 组合；直接在本页配好，保存即可）──", 11, Ui.FAINT, false));
        for (int i = 0; i < clauseBoxes.size(); i++) {
            final int k = i;
            final CondBox b = clauseBoxes.get(i);
            LinearLayout card = Ui.col(c);
            card.setBackground(Ui.blockBg(c, Ui.COND_BAR, 0x0AFFFFFF));
            card.setPadding(Ui.dp(c, 10), Ui.dp(c, 6), Ui.dp(c, 10), Ui.dp(c, 6));
            LinearLayout head = Ui.row(c);
            head.setGravity(Gravity.CENTER_VERTICAL);
            final TextView jb = Ui.chip(c, b.data.a1 == Ops.J_OR ? "或" : "且", Ui.ACCENT);
            jb.setClickable(true);
            jb.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    b.data.a1 = (b.data.a1 == Ops.J_OR) ? Ops.J_AND : Ops.J_OR;
                    jb.setText(b.data.a1 == Ops.J_OR ? "或" : "且");
                }
            });
            head.addView(jb);
            head.addView(Ui.tv(c, "  子条件 " + (k + 1), 12.5f, Ui.DIM, false),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView del = Ui.btnFlat(c, "✕", Ui.WARN);
            del.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { clauseBoxes.remove(k); paintClauses(); }
            });
            head.addView(del);
            card.addView(head);
            b.build(card);
            clauseBox.addView(card);
        }
        if (clauseBoxes.size() + 1 < Ops.CLAUSE_MAX) {
            clauseAddBtn = Ui.btnFlat(c, "＋ 加子条件", Ui.ACCENT);
            TextView addc = clauseAddBtn;
            addc.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    Ops.Step s = new Ops.Step();
                    s.type = Ops.T_CLAUSE;
                    s.a1 = Ops.J_AND;                      // 默认「且」；连接词点左边胶囊切换
                    CondBox b = new CondBox(2, s);
                    b.vJt = 0; b.vRef = "";
                    clauseBoxes.add(b);
                    paintClauses();
                }
            });
            clauseBox.addView(addc);
        } else {
            clauseAddBtn = null;
            clauseBox.addView(Ui.tv(c, "子条件已达上限（含头子句共 " + Ops.CLAUSE_MAX + " 条）", 11.5f, Ui.FAINT, false));
        }
    }

    /* ---------------- v6C：判定编辑器（CondBox） ---------------- */

    /** 一套完整判定编辑 UI + 值（v6C 同页多实例：头子句 1 箱 + 每条子条件 1 箱）。
     *  slot 字段投影：0=14 头（a1=jt a2=p1 a3=p2 a4=取反）/ 1=12 模式3 头（a2=jt a3=p1 a4=p2 j2=取反）/
     *  2=19 子条件（同 1，a1=连接 不动）。data = 值载体（头 = work；子条件 = 各自 Ops.Step）。 */
    private final class CondBox {
        final int slot;
        final Ops.Step data;
        // 值缓存（load 初值 / harvest 回收；read 校验通过后刷新）
        int vJt, vP1, vP2, vMs, vPack, vInv;
        String vRef = "", vRegion = "";
        // UI 件
        Forms.Seg kind, cmp, colorMode2;
        LinearLayout dyn, singleWrap2, multiWrap2;
        Forms.Num p1Num, p2Num, msNum, rNum, gNum, bNum, tolNum;
        Forms.Chips refC2, regC2;
        Forms.Text refT2;
        Forms.SwitchRow chk2, inv2;
        View colorPrev2;
        android.widget.ImageView refThumb2;
        String refLabel2 = "区域 id";

        CondBox(int slot, Ops.Step data) { this.slot = slot; this.data = data; }

        /** 从 data 投影初值（build 前调用）。 */
        void load() {
            int jt = (slot == 0) ? data.a1 : data.a2;
            vJt = (jt >= 0 && jt <= 4) ? jt : 0;
            vP1 = (slot == 0) ? data.a2 : data.a3;
            vP2 = (slot == 0) ? data.a3 : data.a4;
            vInv = (((slot == 0) ? data.a4 : data.j2) == 1) ? 1 : 0;
            vMs = data.ms; vPack = data.j1;
            vRef = data.ref == null ? "" : data.ref;
            vRegion = data.expr == null ? "" : data.expr;
        }

        /** 构建 UI 到 host（判定类型 Seg + 子字段 + 取反开关）。 */
        void build(LinearLayout host) {
            kind = new Forms.Seg(c, new String[]{"区域判断", "开关判断", "找图", "找色", "变量"},
                    (vJt >= 0 && vJt <= 4) ? vJt : 0, new Runnable() {
                public void run() { harvest(); paintDyn(); }
            });
            host.addView(kind.row);
            dyn = Ui.col(c);
            host.addView(dyn);
            inv2 = Forms.switchRow(c, "取反（非）", "对整个判定结果取反（同代码的 ! ）", vInv == 1, null);
            host.addView(inv2.row);
            paintDyn();
        }

        /** 重建本类型子字段（从缓存渲染）。 */
        void paintDyn() {
            dyn.removeAllViews();
            p1Num = p2Num = msNum = null;
            rNum = gNum = bNum = tolNum = null;
            chk2 = null; colorMode2 = null; singleWrap2 = multiWrap2 = null; colorPrev2 = null;
            cmp = null; refC2 = null; refT2 = null; regC2 = null; refThumb2 = null;
            int jt = kind.selected();
            if (jt == 0) {
                p1Num = num2(dyn, "判定点 x", vP1, true);
                p2Num = num2(dyn, "判定点 y", vP2, true);
                pickXY2(p1Num, p2Num);
                dyn.addView(varHint());
                addRefRow2(dyn, false);
            } else if (jt == 1) {
                addRefRow2(dyn, true);
            } else if (jt == 2) {
                addNamePick2(dyn, "模板（.tmpl，必选）", tmplNames, "模板名", 1);
                addRegionPick2(dyn);
                p1Num = num2(dyn, "阈值（0..255）", (vP1 >= 0 && vP1 <= 255) ? vP1 : 8, false);
                chk2 = Forms.switchRow(c, "颜色校验", "灰度命中后比对命中区域平均色（旧模板跳过）", vP2 == 1, null);
                dyn.addView(chk2.row);
                msNum = num2(dyn, "持续查找超时 ms（0 = 单次）", vMs, false);
            } else if (jt == 3) {
                final int p1v = vP1;
                colorMode2 = new Forms.Seg(c, new String[]{"单点（颜色+容差）", "多点（点集）"}, p1v == 1 ? 1 : 0,
                        new Runnable() { public void run() { paintColor2(); } });
                dyn.addView(colorMode2.row);
                singleWrap2 = Ui.col(c);
                int color = (vPack >>> 8) & 0xFFFFFF;
                rNum = num2(singleWrap2, "R（0..255）", (color >> 16) & 0xFF, false);
                gNum = num2(singleWrap2, "G（0..255）", (color >> 8) & 0xFF, false);
                bNum = num2(singleWrap2, "B（0..255）", color & 0xFF, false);
                tolNum = num2(singleWrap2, "容差（0..255）", vPack & 0xFF, false);
                colorPrev2 = new View(c);
                LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT, Ui.dp(c, 10));
                plp.topMargin = Ui.dp(c, 2);
                colorPrev2.setLayoutParams(plp);
                colorPrev2.setBackground(Ui.strokedRound(c, 0xFF000000 | color, Ui.CARD_LINE, 4));
                singleWrap2.addView(colorPrev2);
                TextWatcher tw = new TextWatcher() {
                    public void beforeTextChanged(CharSequence s, int st, int cnt, int af) { }
                    public void onTextChanged(CharSequence s, int st, int b, int cnt) { }
                    public void afterTextChanged(Editable s) { paintColorPrev2(); }
                };
                rNum.field.addTextChangedListener(tw);
                gNum.field.addTextChangedListener(tw);
                bNum.field.addTextChangedListener(tw);
                dyn.addView(singleWrap2);
                multiWrap2 = Ui.col(c);
                addNamePick2(multiWrap2, "点集（.pts，多点必选）", ptsNames, "点集名", 2);
                dyn.addView(multiWrap2);
                addRegionPick2(dyn);
                msNum = num2(dyn, "持续查找超时 ms（0 = 单次）", vMs, false);
                paintColor2();
            } else {
                p1Num = num2(dyn, "变量（点「变量」选择；未写值运行时会中止）",
                        (vP1 <= Ops.V_TDX && vP1 >= Ops.V_CUSTOM15) ? vP1 : -6, true);
                cmp = new Forms.Seg(c, Ops.CMP_SYMS, (vPack >= 0 && vPack <= 5) ? vPack : 0,
                        new Runnable() { public void run() { } });
                dyn.addView(cmp.row);
                p2Num = num2(dyn, "参照值（整数）", vP2, false);
                dyn.addView(Ui.tv(c, "变量值 vs 参照值：成立 / 不成立两分支；被比较变量无值（未写）时中止。",
                        11.5f, Ui.DIM, false));
            }
        }

        private void paintColor2() {
            boolean multi = colorMode2 != null && colorMode2.selected() == 1;
            if (singleWrap2 != null) singleWrap2.setVisibility(multi ? View.GONE : View.VISIBLE);
            if (multiWrap2 != null) multiWrap2.setVisibility(multi ? View.VISIBLE : View.GONE);
        }

        private void paintColorPrev2() {
            if (colorPrev2 == null) return;
            int r = clamp255(peek(rNum)), g = clamp255(peek(gNum)), b = clamp255(peek(bNum));
            colorPrev2.setBackground(Ui.strokedRound(c, 0xFF000000 | (r << 16) | (g << 8) | b, Ui.CARD_LINE, 4));
        }

        /** 宽容回收（列表重建 / 语义切换前调用；不校验、不抛错——保打字中的值）。 */
        void harvest() {
            if (kind == null) return;
            vJt = kind.selected();
            try {
                if (vJt == 0) { vP1 = iv(p1Num); vP2 = iv(p2Num); vRef = currentRef2(); }
                else if (vJt == 1) { vRef = currentRef2(); }
                else if (vJt == 2) {
                    vRef = currentRef2(); vRegion = readRegion2();
                    vP1 = iv(p1Num);
                    vP2 = (chk2 != null && chk2.sw.isChecked()) ? 1 : 0;
                    vMs = iv(msNum);
                } else if (vJt == 3) {
                    vP1 = (colorMode2 != null) ? colorMode2.selected() : vP1;
                    if (vP1 == 1) { vRef = currentRef2(); vPack = 0; }
                    else {
                        int r = clamp255(peek(rNum)), g = clamp255(peek(gNum)), b = clamp255(peek(bNum));
                        vPack = (((r << 16) | (g << 8) | b) << 8) | clamp255(peek(tolNum));
                    }
                    vRegion = readRegion2();
                    vMs = iv(msNum);
                } else {
                    vP1 = iv(p1Num);
                    vPack = (cmp != null) ? cmp.selected() : vPack;
                    vP2 = iv(p2Num);
                }
            } catch (Exception ig) { }                     // 只保能读到的（重建不阻塞）
            if (inv2 != null) vInv = inv2.sw.isChecked() ? 1 : 0;
        }

        /** 校验读回并写入 data（失败标红抛 Bad —— 由调用方收口）。 */
        void read() {
            int jt = (kind != null) ? kind.selected() : 0;
            vJt = jt;
            vInv = (inv2 != null && inv2.sw.isChecked()) ? 1 : 0;
            if (jt == 0) {
                vP1 = ivCoord(p1Num, true, "判定点 x");
                vP2 = ivCoord(p2Num, false, "判定点 y");
                vRef = readRef2(); vRegion = ""; vMs = 0; vPack = 0;
            } else if (jt == 1) {
                vRef = readRef2(); vRegion = ""; vP1 = 0; vP2 = 0; vMs = 0; vPack = 0;
            } else if (jt == 2) {
                vRef = readRef2(); vRegion = readRegion2();
                vP1 = ivField(p1Num, 0, 255, false, "阈值");
                vP2 = (chk2 != null && chk2.sw.isChecked()) ? 1 : 0;
                vMs = ivField(msNum, 0, 60000, false, "超时"); vPack = 0;
            } else if (jt == 3) {
                vP1 = (colorMode2 != null && colorMode2.selected() == 1) ? 1 : 0;   // v6C 修：模式写回 a1（此前从未写 → 多点必挂）
                if (vP1 == 1) { vRef = readRef2(); vPack = 0; }
                else {
                    vRef = "";
                    int r = ivField(rNum, 0, 255, false, "R");
                    int g = ivField(gNum, 0, 255, false, "G");
                    int b = ivField(bNum, 0, 255, false, "B");
                    int tol = ivField(tolNum, 0, 255, false, "容差");
                    vPack = (((r << 16) | (g << 8) | b) << 8) | tol;
                }
                vRegion = readRegion2();
                vMs = ivField(msNum, 0, 60000, false, "超时");
                vP2 = 0;
            } else {
                vP1 = iv(p1Num);
                if (vP1 > Ops.V_TDX || vP1 < Ops.V_CUSTOM15) {
                    p1Num.setError("变量编码非法（-25..-1）——点「变量」选择");
                    throw new Bad();
                }
                p1Num.clearError();
                vPack = (cmp != null && cmp.selected() >= 0 && cmp.selected() <= 5) ? cmp.selected() : 0;
                vP2 = iv(p2Num);
                vRef = ""; vRegion = ""; vMs = 0;
            }
            write();
        }

        /** 值 → data（slot 投影）。 */
        private void write() {
            if (slot == 0) {
                data.a1 = vJt; data.a2 = vP1; data.a3 = vP2; data.a4 = vInv;
            } else {
                data.a2 = vJt; data.a3 = vP1; data.a4 = vP2; data.j2 = vInv;
            }
            data.ms = vMs;
            data.ref = vRef;
            data.expr = vRegion;
            data.j1 = vPack;
        }

        /* ---- 子件构造（页内帮助函数的箱内版） ---- */

        private Forms.Num num2(LinearLayout host, String label, int val, boolean var) {
            Forms.Num n = new Forms.Num(c, label, val);
            if (var) n.enableVar(customVarNames(), customVarCodes());
            host.addView(n.row);
            return n;
        }

        private void pickXY2(Forms.Num x, Forms.Num y) {
            if (visual == null || x == null) return;
            final Forms.Num fx = x;
            final Forms.Num fy = y;
            fx.enablePick("取点 x,y", new Runnable() {
                public void run() { visual.pickPointXY(fx, fy); }
            });
        }

        private String currentRef2() {
            if (refC2 != null) return refC2.selected();
            if (refT2 != null) return refT2.value();
            return vRef == null ? "" : vRef;
        }

        private String readRef2() {
            String r = currentRef2();
            if (r == null || r.isEmpty()) {
                if (refT2 != null) refT2.setError("必填：" + refLabel2);
                throw new Bad();
            }
            if (!Regions.idOk(r)) {
                if (refT2 != null) refT2.setError(refLabel2 + "非法（1..15 个可见字符）");
                throw new Bad();
            }
            if (refT2 != null) refT2.clearError();
            return r;
        }

        private void addRefRow2(LinearLayout host, boolean needSwitch) {
            List<String> base = needSwitch ? switchIds : regionIds;
            refLabel2 = needSwitch ? "开关型区域 id" : "区域 id";
            if (base != null && !base.isEmpty()) {
                host.addView(Ui.label(c, needSwitch ? "开关型区域 id（仅开关型）" : "区域 id"));
                List<String> ids = new ArrayList<String>(base);
                if (vRef != null && !vRef.isEmpty() && !ids.contains(vRef)) ids.add(0, vRef);
                String sel = (vRef != null && ids.contains(vRef)) ? vRef : ids.get(0);
                refC2 = new Forms.Chips(c, ids, sel, null);
                host.addView(refC2.view);
            } else {
                refT2 = new Forms.Text(c,
                        needSwitch ? "开关型区域 id（暂无开关型区域，先去区域编辑打开「开关型」）"
                                   : "区域 id（如 c1）",
                        vRef == null ? "" : vRef, "");
                host.addView(refT2.row);
            }
        }

        private String readRegion2() {
            if (regC2 != null) {
                String s = regC2.selected();
                return "（全屏）".equals(s) ? "" : s;
            }
            return vRegion == null ? "" : vRegion;
        }

        private void addRegionPick2(LinearLayout host) {
            List<String> base = new ArrayList<String>();
            base.add("（全屏）");
            if (regionIds != null) base.addAll(regionIds);
            String cur = vRegion == null ? "" : vRegion;
            if (!cur.isEmpty() && !base.contains(cur)) base.add(0, cur);
            String sel = cur.isEmpty() ? "（全屏）" : cur;
            host.addView(Ui.label(c, "区域限定（全屏 = 不限）"));
            regC2 = new Forms.Chips(c, base, sel, null);
            host.addView(regC2.view);
        }

        private void refreshRefThumb2() {
            loadThumb(refThumb2, currentRef2());
        }

        /** 模板 / 点集名选择（箱内版；kind 1=模板 2=点集，带素材入口与缩略图）。 */
        private void addNamePick2(LinearLayout host, String label, List<String> names, String what, int kindN) {
            host.addView(Ui.label(c, label));
            refLabel2 = what;
            refThumb2 = null;
            AssetSelector selector = new AssetSelector(host, names, vRef, new Runnable() {
                public void run() { refreshRefThumb2(); }
            });
            refC2 = selector.chips;
            refT2 = selector.text;
            String sel = selector.selected;
            if (visual == null) return;
            if (kindN == 1) refThumb2 = createAssetThumbnail(host, sel);
            LinearLayout acts = Ui.row(c);
            acts.setPadding(0, Ui.dp(c, 4), 0, Ui.dp(c, 2));
            if (kindN == 1) {
                TextView bNew = Ui.btnSmall(c, "＋ 新建模板");
                bNew.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { visual.newTmpl(assetTarget2()); }
                });
                TextView bEdit = Ui.btnSmall(c, "编辑");
                bEdit.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { visual.editTmpl(currentRef2()); }
                });
                TextView bTest = Ui.btnSmall(c, "试一下");
                bTest.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        int th = 8;
                        try {
                            if (p1Num != null) {
                                int v2 = p1Num.value();
                                if (v2 >= 0) th = v2;
                            }
                        } catch (Exception ig) { }
                        boolean cc = chk2 != null && chk2.sw.isChecked();
                        String reg = regC2 != null ? regC2.selected() : "";
                        if ("（全屏）".equals(reg)) reg = "";
                        visual.testTmpl(currentRef2(), th, cc, reg);
                    }
                });
                addAssetActions(host, acts, bNew, bEdit, bTest);
            } else {
                TextView bNew = Ui.btnSmall(c, "＋ 新建点集");
                bNew.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { visual.newPts(assetTarget2()); }
                });
                TextView bEdit = Ui.btnSmall(c, "编辑");
                bEdit.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { visual.editPts(currentRef2()); }
                });
                TextView bTest = Ui.btnSmall(c, "试一下");
                bTest.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        String reg = regC2 != null ? regC2.selected() : "";
                        if ("（全屏）".equals(reg)) reg = "";
                        visual.testPts(currentRef2(), reg);
                    }
                });
                addAssetActions(host, acts, bNew, bEdit, bTest);
            }
        }

        private AssetTarget assetTarget2() {
            return new AssetTarget() {
                public void onSaved(String name) {
                    if (refC2 != null) { refC2.addOption(name); refC2.setSelected(name); }
                    else if (refT2 != null) refT2.field.setText(name);
                    if (refThumb2 != null) refThumb2.setVisibility(View.VISIBLE);
                    refreshRefThumb2();
                }
            };
        }
    }

    /** v6B/v6C：块头子条件工作列表（每箱 read() 后收集；非块头页 = null）。仅供 collect()==null 后读取。 */
    public java.util.List<Ops.Step> collectedClauses() {
        if (clauseBoxes == null) return null;
        java.util.List<Ops.Step> l = new java.util.ArrayList<Ops.Step>();
        for (CondBox b : clauseBoxes) l.add(b.data);
        return l;
    }

    /** 测试台直驱：本页子条件箱数量（v6C）。 */
    public int clauseBoxCountForTest() { return clauseBoxes == null ? 0 : clauseBoxes.size(); }

    /** 测试台直驱：点第 boxIdx 条子条件的第 kind 个判定类型（真实点按路径）；不可用返回 false。 */
    public boolean clauseTapForTest(int boxIdx, int kind) {
        if (clauseBoxes == null || boxIdx < 0 || boxIdx >= clauseBoxes.size()) return false;
        CondBox b = clauseBoxes.get(boxIdx);
        if (b.kind == null || kind < 0) return false;
        View v = b.kind.row.getChildAt(kind);
        if (v == null) return false;
        v.performClick();
        return true;
    }

    /** v32：测试台直驱——点 [＋ 加子条件]（真实按钮路径；达上限/未建区返回 false）。 */
    public boolean clauseAddForTest() {
        if (clauseAddBtn == null) return false;
        clauseAddBtn.performClick();
        return true;
    }

    /** v32：判定行（判定类型 Seg）格宽体检——逐格比较「可用宽 vs 文本宽」，防 4 字标签被裁尾字。
     *  返回 "适配(最紧余量 Npx)" 或 "裁字:「X」…"；需在渲染/量测后进行。 */
    public String segFitDetailForTest() {
        java.util.List<Forms.Seg> segs = new java.util.ArrayList<Forms.Seg>();
        if (headerBox != null && headerBox.kind != null) segs.add(headerBox.kind);
        if (clauseBoxes != null) {
            for (CondBox b : clauseBoxes) if (b.kind != null) segs.add(b.kind);
        }
        float minMargin = Float.MAX_VALUE;
        String bad = null;
        for (Forms.Seg sg : segs) {
            for (int i = 0; i < sg.itemsCountForTest(); i++) {
                TextView tv = sg.itemForTest(i);
                if (tv.getWidth() == 0 || tv.getText().length() == 0) continue;      // 未量测：跳过
                float tw = tv.getPaint().measureText(tv.getText().toString());
                float avail = tv.getWidth() - tv.getPaddingLeft() - tv.getPaddingRight();
                float margin = avail - tw;
                if (margin < minMargin) minMargin = margin;
                if (tw > avail + 1.5f && bad == null) bad = "「" + tv.getText() + "」格 " + Math.round(avail) + " < 文本 " + Math.round(tw);
            }
        }
        if (minMargin == Float.MAX_VALUE) return "未量测";
        return (bad == null ? "适配(最紧余量 " + Math.round(minMargin) + "px)"
                            : "裁字: " + bad + "（最紧余量 " + Math.round(minMargin) + "px）");
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
        if (refThumb == null) return;
        loadThumb(refThumb, currentRef());
    }

    private void loadThumb(final android.widget.ImageView th, final String name) {
        if (th == null || name == null || name.isEmpty()) return;
        th.setTag(name);
        th.setImageDrawable(null);
        Ui.bg(new Runnable() {
            public void run() {
                final android.graphics.Bitmap bmp = Assets.thumbTmpl(name, Ui.dp(c, 96));
                Ui.MAIN.post(new Runnable() {
                    public void run() { if (name.equals(th.getTag())) th.setImageBitmap(bmp); }
                });
            }
        }, null);
    }

    private TextView varHint() {
        TextView t = Ui.tv(c, "数字可用「变量」按钮填（触发点 / 命中点 fx·fy / 自定义变量），选定后显示变量名；也可直接输入负数",
                11, Ui.DIM, false);
        t.setPadding(0, Ui.dp(c, 2), 0, Ui.dp(c, 2));
        return t;
    }

    /** 计算步编辑器：变量名 + 表达式 + 插入 chips（变量 / 函数 / 运算符）。 */
    private void addCalcEditor() {
        String initName = work.ref;
        if (initName == null || initName.isEmpty()) initName = autoVarName();
        calcName = new Forms.Text(c, "变量名（1..15 字，可用中文）", initName, "如 dx");
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

        LinearLayout colorExtra = Ui.col(c);
        addRegionPick(colorExtra);
        nMs = num(colorExtra, "持续查找超时 ms（0 = 单次）", work.ms, false);
        addAdvanced(dyn, colorExtra, work.ms != 0 || (work.expr != null && !work.expr.isEmpty()));
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

    /** 共用素材选择器；无素材时保留手工输入，避免失效引用被静默替换。 */
    private final class AssetSelector {
        Forms.Chips chips;
        Forms.Text text;
        String selected;
        AssetSelector(LinearLayout host, List<String> names, String current, Runnable onPick) {
            String value = current == null ? "" : current;
            if (names != null && !names.isEmpty()) {
                List<String> ids = new ArrayList<String>(names);
                if (!value.isEmpty() && !ids.contains(value)) ids.add(0, value);
                selected = ids.contains(value) ? value : ids.get(0);
                chips = new Forms.Chips(c, ids, selected, onPick);
                host.addView(chips.view);
            } else {
                text = new Forms.Text(c, "（无可选项时手工输入）", value, "");
                host.addView(text.row);
            }
        }
    }

    private void addAssetActions(LinearLayout host, LinearLayout row,
            TextView create, TextView edit, TextView test) {
        row.addView(create);
        row.addView(edit);
        row.addView(test);
        host.addView(row);
    }

    private android.widget.ImageView createAssetThumbnail(LinearLayout host, String name) {
        android.widget.ImageView thumb = new android.widget.ImageView(c);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(Ui.dp(c, 96), Ui.dp(c, 96));
        lp.topMargin = Ui.dp(c, 4);
        thumb.setLayoutParams(lp);
        thumb.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
        thumb.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
        thumb.setContentDescription("模板预览");
        thumb.setVisibility(name == null || name.isEmpty() ? View.GONE : View.VISIBLE);
        host.addView(thumb);
        if (name != null && !name.isEmpty()) loadThumb(thumb, name);
        return thumb;
    }

    /** 模板 / 点集名选择（列表为空时退化为手工输入）；kind 1=模板 2=点集（带素材入口）。 */
    private void addNamePick(LinearLayout host, String label, List<String> names, String what, int kind) {
        host.addView(Ui.label(c, label));
        refLabel = what;
        refThumb = null;
        AssetSelector selector = new AssetSelector(host, names, work.ref, new Runnable() {
            public void run() { refreshRefThumb(); }
        });
        refC = selector.chips;
        refT = selector.text;
        String sel = selector.selected;
        if (visual == null) return;
        if (kind == 1) refThumb = createAssetThumbnail(host, sel);
        LinearLayout acts = Ui.row(c);
        acts.setPadding(0, Ui.dp(c, 4), 0, Ui.dp(c, 2));
        if (kind == 1) {
            TextView bNew = Ui.btnSmall(c, "＋ 新建模板");
            bNew.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { visual.newTmpl(assetTarget()); }
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
            addAssetActions(host, acts, bNew, bEdit, bTest);
        } else {
            TextView bNew = Ui.btnSmall(c, "＋ 新建点集");
            bNew.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { visual.newPts(assetTarget()); }
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
            addAssetActions(host, acts, bNew, bEdit, bTest);
        }
    }

    private AssetTarget assetTarget() {
        return new AssetTarget() {
            public void onSaved(String name) {
                if (refC != null) { refC.addOption(name); refC.setSelected(name); }
                else if (refT != null) refT.field.setText(name);
                if (refThumb != null) refThumb.setVisibility(View.VISIBLE);
                refreshRefThumb();
            }
        };
    }

    /** 视觉步「区域限定」（空 = 全屏；复用 expr 列）。 */
    private void addRegionPick() { addRegionPick(dyn); }

    /** host 版（v12/v6C：判定子编辑器经 CondBox 的箱内版；此版留旧调用面）。 */
    private void addRegionPick(LinearLayout host) {
        List<String> base = new ArrayList<String>();
        base.add("（全屏）");
        if (regionIds != null) base.addAll(regionIds);
        String cur = work.expr == null ? "" : work.expr;
        if (!cur.isEmpty() && !base.contains(cur)) base.add(0, cur);   // 旧值保留展示
        String sel = cur.isEmpty() ? "（全屏）" : cur;
        host.addView(Ui.label(c, "区域限定（全屏 = 不限）"));
        regC = new Forms.Chips(c, base, sel, null);
        host.addView(regC.view);
    }

    /** 区域 id 选择行：区域判断 = 任意区域；开关判断 = 仅开关型。 */
    private void addRefRow(boolean needSwitch) { addRefRow(dyn, needSwitch); }

    /** host 版（v12/v6C：判定子编辑器经 CondBox 的箱内版；此版留旧调用面）。 */
    private void addRefRow(LinearLayout host, boolean needSwitch) {
        List<String> base = needSwitch ? switchIds : regionIds;
        refLabel = needSwitch ? "开关型区域 id" : "区域 id";
        if (base != null && !base.isEmpty()) {
            host.addView(Ui.label(c, needSwitch ? "开关型区域 id（仅开关型）" : "区域 id"));
            List<String> ids = new ArrayList<String>(base);
            if (work.ref != null && !work.ref.isEmpty() && !ids.contains(work.ref)) {
                ids.add(0, work.ref);   // 旧值 / 已失效：保留展示，保存时校验
            }
            String sel = (work.ref != null && ids.contains(work.ref)) ? work.ref : ids.get(0);
            refC = new Forms.Chips(c, ids, sel, null);
            host.addView(refC.view);
        } else {
            refT = new Forms.Text(c,
                    needSwitch ? "开关型区域 id（暂无开关型区域，先去区域编辑打开「开关型」）"
                               : "区域 id（如 c1）",
                    work.ref == null ? "" : work.ref, "");
            host.addView(refT.row);
        }
    }

    /** Seg 位序 → 档位码（v12.1：继续 / 跳过 / 跳转 / 中止，最常用放最前）。 */
    private static final int[] TIER4 = {Ops.C_CONT, Ops.C_SKIP, Ops.C_JUMP, Ops.C_ABORT};
    /** 档位码 → Seg 位序（TIER4 的反表）。 */
    private static final int[] TIER_SEG = {3, 1, 0, 2};

    private void addTierPair() {
        final int t4 = (work.a4 >= 0 && work.a4 <= 3) ? work.a4 : 0;
        final int t3 = (work.a3 >= 0 && work.a3 <= 3) ? work.a3 : 0;

        j1Wrap = Ui.col(c);
        j1Wrap.setVisibility(t4 == Ops.C_JUMP ? View.VISIBLE : View.GONE);
        nJ1 = num(j1Wrap, "跳转目标步（0 = 结束）", work.j1, false);
        tierA4 = new Forms.Seg(c, new String[]{"继续", "跳过", "跳转", "中止"}, TIER_SEG[t4], new Runnable() {
            public void run() {
                j1Wrap.setVisibility(tierA4.selected() == 2 ? View.VISIBLE : View.GONE);
            }
        });
        dyn.addView(tierSide("成立时（命中 / 条件为真）", Ui.ACCENT, tierA4, j1Wrap));

        j2Wrap = Ui.col(c);
        j2Wrap.setVisibility(t3 == Ops.C_JUMP ? View.VISIBLE : View.GONE);
        nJ2 = num(j2Wrap, "跳转目标步（0 = 结束）", work.j2, false);
        tierA3 = new Forms.Seg(c, new String[]{"继续", "跳过", "跳转", "中止"}, TIER_SEG[t3], new Runnable() {
            public void run() {
                j2Wrap.setVisibility(tierA3.selected() == 2 ? View.VISIBLE : View.GONE);
            }
        });
        dyn.addView(tierSide("不成立时（未命中 / 条件为假）", 0xFF49515C, tierA3, j2Wrap));
    }

    /** 分支小卡（v12.1 与色带块呼应）：左 3dp 条 + 微底 + 标签 + 档位 Seg + 跳转框。 */
    private LinearLayout tierSide(String label, int barColor, Forms.Seg seg, LinearLayout jw) {
        LinearLayout card = Ui.col(c);
        card.setBackground(Ui.blockBg(c, barColor, 0x0AFFFFFF));
        card.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
        card.addView(Ui.label(c, label));
        card.addView(seg.row);
        card.addView(jw);
        return card;
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
            int v = n.value();                       // v32：支持变量名（fx / dx…）→ 编码解析
            n.clearError();
            return v;
        } catch (Exception e) {
            n.setError("必须是整数（可为负）或变量名");
            revealField(n.field);
            throw new Bad();
        }
    }

    /** 字面值在 [lo,hi]；allowVar = 同时接受负数编码 -25..-1。 */
    private int ivField(Forms.Num n, int lo, int hi, boolean allowVar, String what) {
        int v = iv(n);
        boolean ok = (v >= lo && v <= hi) || (allowVar && v >= Ops.V_CUSTOM15 && v <= Ops.V_TDX);
        if (!ok) {
            n.setError(what + "需在 " + lo + ".." + hi + (allowVar ? " 或变量" : ""));
            revealField(n.field);
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
            revealField(n.field);
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
            if (refT != null) refT.setError(refLabel + "非法（1..15 个可见字符）");
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
                case Ops.T_TOAST:
                    work.a1 = work.a2 = work.a3 = work.a4 = work.j1 = work.j2 = 0;
                    work.ref = "";
                    work.ms = ivField(nMs, 0, 60000, false, "显示时长");
                    work.expr = (tvE != null) ? tvE.getText().toString() : "";
                    if (work.expr.isEmpty()) {
                        if (tvE != null) tvE.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.WARN, 6));
                        throw new Bad();
                    }
                    {
                        String te = Ops.toastTplErr(work.expr, customVarNames());
                        if (te != null) {
                            if (tvE != null) tvE.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.WARN, 6));
                            throw new Bad();
                        }
                    }
                    break;
                case Ops.T_HL:
                    work.a1 = work.a2 = work.a3 = work.a4 = work.j1 = work.j2 = 0;
                    work.expr = "";
                    work.ms = ivField(nMs, 0, 60000, false, "显示时长");
                    if (hlChips != null) work.ref = hlChips.selected();
                    if (work.ref == null || work.ref.isEmpty() || !Regions.idOk(work.ref)) throw new Bad();
                    break;
                case Ops.T_BOX:
                    work.a1 = ivCoord(nA1, true, "x1");
                    work.a2 = ivCoord(nA2, false, "y1");
                    work.a3 = ivCoord(nA3, true, "x2");
                    work.a4 = ivCoord(nA4, false, "y2");
                    work.ms = ivField(nMs, 0, 60000, false, "显示时长");
                    work.ref = ""; work.expr = ""; work.j1 = work.j2 = 0;
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
                    work.a4 = TIER4[tierA4.selected()];
                    work.a3 = TIER4[tierA3.selected()];
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                case Ops.T_COND_TOGGLE:
                    work.ref = readRef();
                    work.a4 = TIER4[tierA4.selected()];
                    work.a3 = TIER4[tierA3.selected()];
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                case Ops.T_JUMP:
                    work.a1 = iv(nA1);
                    break;
                case Ops.T_CALC: {
                    String nm = calcName.value();
                    if (!Ops.varNameOk(nm)) {
                        calcName.setError("变量名非法（1..15 个可见字符，首字符不可数字）");
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
                    work.a4 = TIER4[tierA4.selected()];
                    work.a3 = TIER4[tierA3.selected()];
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
                    work.a4 = TIER4[tierA4.selected()];
                    work.a3 = TIER4[tierA3.selected()];
                    if (work.a4 == Ops.C_JUMP) work.j1 = iv(nJ1);
                    if (work.a3 == Ops.C_JUMP) work.j2 = iv(nJ2);
                    break;
                }
                case Ops.T_LOOP: {
                    int mode = loopMode.selected();
                    work.a1 = mode;
                    work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
                    work.ref = ""; work.expr = "";
                    if (mode == 0) {
                        work.a2 = ivField(nA2, 1, 99999, false, "次数");
                    } else if (mode == 1) {
                        work.a2 = ivField(nA2, 1000, 86400000, false, "时长 ms");
                    } else if (mode == 3) {
                        if (headerBox != null) headerBox.read();   // v6C：CondBox 读回（字段错由本方法 catch 收口）
                    }
                    break;
                }
                case Ops.T_IF:
                    work.a1 = work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
                    work.ref = ""; work.expr = "";
                    if (headerBox != null) headerBox.read();
                    break;
                case Ops.T_BREAK:
                case Ops.T_CONT: {
                    int dv = ivField(nDepth, 1, 8, false, "层数");
                    work.a1 = (dv <= 1) ? 0 : dv;      // 1 层写 0（默认；兼容口径）
                    work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
                    work.ref = ""; work.expr = "";
                    break;
                }
                case Ops.T_STOP:
                    work.a1 = (stopMode != null && stopMode.selected() == 1) ? 1 : 0;
                    work.a2 = work.a3 = work.a4 = work.ms = work.j1 = work.j2 = 0;
                    work.ref = ""; work.expr = "";
                    break;
                case Ops.T_CLAUSE:
                    work.a1 = (joinMode != null && joinMode.selected() == 1) ? Ops.J_OR : Ops.J_AND;
                    if (headerBox != null) headerBox.read();
                    break;
                default:
                    break;
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /* v6C：readCond 已并入 CondBox.read()（槽位投影写在箱内 write()）。 */

    /** 测试台直驱：点本页主判定编辑器的第 kind 个判定类型（真实点按路径：pick + 回调重建子配置）；不可用时返回 false。 */
    public boolean condTapForTest(int kind) {
        if (headerBox == null || headerBox.kind == null || kind < 0) return false;
        View v = headerBox.kind.row.getChildAt(kind);
        if (v == null) return false;
        v.performClick();
        return true;
    }

    /* ---- v6D：提示编辑器辅助 ---- */

    /** 插入变量条（目标 = 指定输入框；点一条插入 `{名}`）。 */
    private HorizontalScrollView chipStripBraced(List<String> names, final EditText target) {
        HorizontalScrollView sc = new HorizontalScrollView(c);
        sc.setHorizontalScrollBarEnabled(false);
        LinearLayout strip = Ui.row(c);
        strip.setPadding(0, Ui.dp(c, 2), 0, Ui.dp(c, 2));
        for (String it : names) {
            final String nm = it;
            TextView t = Ui.tv(c, it, 12.5f, Ui.ACCENT, false);
            t.setGravity(Gravity.CENTER);
            t.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
            t.setBackground(Ui.pressableStroked(c, 0x00000000, (Ui.ACCENT & 0x00FFFFFF) | 0x59000000, 0xFF2A3138, 6));
            t.setClickable(true);
            t.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { insertInto(target, "{" + nm + "}"); }
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

    private void insertInto(EditText target, String text) {
        if (target == null) return;
        String cur = target.getText().toString();
        int st = Math.max(0, Math.min(target.getSelectionStart(), cur.length()));
        String nv = cur.substring(0, st) + text + cur.substring(st);
        target.setText(nv);
        target.setSelection(Math.min(nv.length(), st + text.length()));
    }

    /** v6D：提示实时预览（错误优先显示；否则占位按示例值 5 渲染）。 */
    private void refreshToastPreview() {
        if (tvE == null || tvPrev == null) return;
        String t = tvE.getText().toString();
        String err = Ops.toastTplErr(t, customVarNames());
        if (err != null) {
            tvPrev.setTextColor(Ui.WARN);
            tvPrev.setText("⚠ " + err);
        } else {
            tvPrev.setTextColor(Ui.FAINT);
            tvPrev.setText("示例预览（变量按 5 显示）：" + toastSample(t));
        }
    }

    /** 示例渲染（{名} → 5；{{ }} 转义）——纯展示，与核心渲染同规则。 */
    private static String toastSample(String t) {
        StringBuilder sb = new StringBuilder();
        int i = 0, len = t.length();
        while (i < len) {
            char ch = t.charAt(i);
            if (ch == '{' && i + 1 < len && t.charAt(i + 1) == '{') { sb.append('{'); i += 2; continue; }
            if (ch == '}' && i + 1 < len && t.charAt(i + 1) == '}') { sb.append('}'); i += 2; continue; }
            if (ch == '{') {
                int j = t.indexOf('}', i + 1);
                if (j < 0) { sb.append(t.substring(i)); break; }   // 错误态已另行提示
                sb.append('5');
                i = j + 1;
            } else { sb.append(ch); i++; }
        }
        return sb.toString();
    }

    /** 回收并校验：成功返回 null（work 与子条件箱均已写回最新参数）；失败返回提示（出错字段已标红）。 */
    public String collect() {
        if (!read(curType)) return "有字段未通过校验（已标红）";
        if (clauseBoxes != null) {
            for (CondBox b : clauseBoxes) {
                try {
                    b.read();
                } catch (Exception e) {
                    return "有子条件字段未通过校验（已标红）";
                }
            }
        }
        work.type = curType;
        return null;
    }
}
