package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 步骤编辑页（全屏表单，替代原弹窗）：类型宫格选择 + 动态参数（步进器 + 变量选择）+ 行内校验。
 * 口径与原 StepForm 一致：参数写回 work 副本；换类型前先回收当前字段（失败则不切换）。
 */
public final class StepEdit {
    private static final String[] TYPE_NAMES =
            {"点按", "滑动", "等待", "按下", "弹起", "区域判断", "开关判断", "跳转"};

    public final LinearLayout root;

    private final Context c;
    private final Ops.Step work;
    private final List<String> regionIds;    // 全部区域（区域判断用）
    private final List<String> switchIds;    // 仅开关型（开关判断用）
    private int curType;

    private final TextView[] typeChips = new TextView[8];
    private final LinearLayout dyn;

    private Forms.Num nA1, nA2, nA3, nA4, nMs, nJ1, nJ2;
    private Forms.Text refT;
    private Forms.Chips refC;
    private Forms.Seg tierA4, tierA3;
    private LinearLayout j1Wrap, j2Wrap;

    private static final class Bad extends RuntimeException {
        Bad() { super("bad-field"); }
    }

    private StepEdit(Context c, Ops.Step work, List<String> regionIds, List<String> switchIds) {
        this.c = c;
        this.work = work;
        this.regionIds = regionIds;
        this.switchIds = switchIds;
        curType = work.type;
        root = Ui.col(c);
        root.setPadding(Ui.dp(c, 12), Ui.dp(c, 0), Ui.dp(c, 12), Ui.dp(c, 12));

        // ---- 类型宫格（2 行 × 4） ----
        LinearLayout typeCard = Forms.section(c, "步骤类型", root);
        LinearLayout r1 = Ui.row(c);
        LinearLayout r2 = Ui.row(c);
        for (int i = 0; i < 8; i++) {
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
            (i < 4 ? r1 : r2).addView(chip);
        }
        typeCard.addView(r1);
        typeCard.addView(r2);

        // ---- 参数 ----
        LinearLayout paramCard = Forms.section(c, "参数", root);
        dyn = Ui.col(c);
        paramCard.addView(dyn);
        paintTypes();
        build(curType);
    }

    public static StepEdit build(Context c, Ops.Step work, List<String> regionIds, List<String> switchIds) {
        return new StepEdit(c, work, regionIds, switchIds);
    }

    private void paintTypes() {
        for (int i = 0; i < 8; i++) {
            boolean on = i + 1 == curType;
            typeChips[i].setTextColor(on ? 0xFFFFFFFF : Ui.DIM);
            typeChips[i].setTypeface(typeChips[i].getTypeface(),
                    on ? android.graphics.Typeface.BOLD : android.graphics.Typeface.NORMAL);
            typeChips[i].setBackground(on
                    ? Ui.round(c, Ui.ACCENT, 12)
                    : Ui.round(c, Ui.CARD2, 12));
        }
    }

    private void switchType(int t) {
        if (t == curType) return;
        if (!read(curType)) return;    // 先回收当前参数；失败则不切换（出错字段已标红）
        curType = t;
        work.type = t;
        build(t);
        paintTypes();
    }

    // ---------------- 动态字段 ----------------

    private Forms.Num num(String label, int val, boolean var) {
        return num(dyn, label, val, var);
    }

    private Forms.Num num(LinearLayout host, String label, int val, boolean var) {
        Forms.Num n = new Forms.Num(c, label, val);
        if (var) n.enableVar();
        host.addView(n.row);
        return n;
    }

    private void build(int type) {
        dyn.removeAllViews();
        nA1 = nA2 = nA3 = nA4 = nMs = nJ1 = nJ2 = null;
        refT = null;
        refC = null;
        tierA4 = tierA3 = null;
        j1Wrap = j2Wrap = null;
        switch (type) {
            case Ops.T_TAP:
                nA1 = num("x", work.a1, true);
                nA2 = num("y", work.a2, true);
                nMs = num("按住时长 ms", work.ms, true);
                dyn.addView(varHint());
                break;
            case Ops.T_SWIPE:
                nA1 = num("起点 x", work.a1, true);
                nA2 = num("起点 y", work.a2, true);
                nA3 = num("终点 x", work.a3, true);
                nA4 = num("终点 y", work.a4, true);
                nMs = num("时长 ms", work.ms, true);
                dyn.addView(varHint());
                break;
            case Ops.T_WAIT:
                nMs = num("等待 ms", work.ms, false);
                break;
            case Ops.T_DOWN:
                nA1 = num("x", work.a1, true);
                nA2 = num("y", work.a2, true);
                dyn.addView(varHint());
                break;
            case Ops.T_UP:
                dyn.addView(Ui.tv(c, "（无参数）", 13, Ui.DIM, false));
                break;
            case Ops.T_COND_REGION:
                nA1 = num("判定点 x", work.a1, true);
                nA2 = num("判定点 y", work.a2, true);
                dyn.addView(varHint());
                addRefRow();
                addTierPair();
                break;
            case Ops.T_COND_TOGGLE:
                addRefRow();
                addTierPair();
                break;
            case Ops.T_JUMP:
                nA1 = num("目标步（0 = 结束）", work.a1, false);
                break;
            default:
                break;
        }
    }

    private TextView varHint() {
        TextView t = Ui.tv(c, "数字可用“变量”按钮填触摸点变量（-1..-5），也可直接输入负数", 11, Ui.DIM, false);
        t.setPadding(0, Ui.dp(c, 2), 0, Ui.dp(c, 2));
        return t;
    }

    /** 区域 id 选择行：区域判断 = 任意区域；开关判断 = 仅开关型（对齐核心「非开关型」口径）。 */
    private void addRefRow() {
        boolean needSwitch = curType == Ops.T_COND_TOGGLE;
        List<String> base = needSwitch ? switchIds : regionIds;
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

        dyn.addView(Ui.label(c, "成立侧（条件为真）"));
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

        dyn.addView(Ui.label(c, "不成立侧（条件为假）"));
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

    private String readRef() {
        if (refC != null) return refC.selected();
        if (refT != null) {
            String r = refT.value();
            if (r.isEmpty()) {
                refT.setError("必填：区域 id");
                throw new Bad();
            }
            refT.clearError();
            return r;
        }
        return work.ref;
    }

    /** 读当前类型可见字段 → work；出错标红并返回 false。 */
    private boolean read(int type) {
        try {
            switch (type) {
                case Ops.T_TAP:
                    work.a1 = iv(nA1);
                    work.a2 = iv(nA2);
                    work.ms = iv(nMs);
                    break;
                case Ops.T_SWIPE:
                    work.a1 = iv(nA1);
                    work.a2 = iv(nA2);
                    work.a3 = iv(nA3);
                    work.a4 = iv(nA4);
                    work.ms = iv(nMs);
                    break;
                case Ops.T_WAIT:
                    work.ms = iv(nMs);
                    break;
                case Ops.T_DOWN:
                    work.a1 = iv(nA1);
                    work.a2 = iv(nA2);
                    break;
                case Ops.T_UP:
                    break;
                case Ops.T_COND_REGION:
                    work.a1 = iv(nA1);
                    work.a2 = iv(nA2);
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
