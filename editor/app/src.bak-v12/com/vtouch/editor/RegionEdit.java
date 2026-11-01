package com.vtouch.editor;

import android.content.Context;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 区域编辑页（全屏表单，替代原弹窗）：分区卡片（基本/位置/行为/绑定）+
 * 步进数字/分段选择/开关行/胶囊选择 + 行内校验。
 */
public final class RegionEdit {
    public final LinearLayout root;

    private final Forms.Text idRow;
    private final Forms.Seg typeSeg;
    private final Forms.Num n1, n2, n3, n4;
    private final Switch swEn, swKind;
    private final Forms.Chips bindChips;
    private final TextView evLabel;
    private final Forms.Seg evSeg;

    private RegionEdit(Context c, Regions.Region work, List<String> opNames, String frameHint) {
        root = Ui.col(c);
        root.setPadding(Ui.dp(c, 12), Ui.dp(c, 0), Ui.dp(c, 12), Ui.dp(c, 12));

        // ---- 基本 ----
        LinearLayout base = Forms.section(c, "基本", root);
        idRow = new Forms.Text(c, "id（字母/数字/下划线/短横，1..15）", work.id, "如 r1");
        base.addView(idRow.row);
        base.addView(Ui.label(c, "类型"));
        typeSeg = new Forms.Seg(c, new String[]{"矩形", "圆"}, work.type == 1 ? 1 : 0, new Runnable() {
            public void run() { applyType(); }
        });
        base.addView(typeSeg.row);

        // ---- 位置 ----
        LinearLayout pos = Forms.section(c, "位置（像素）", root);
        if (frameHint != null && !frameHint.isEmpty()) {
            TextView hint = Ui.tv(c, frameHint, 11, Ui.DIM, false);
            hint.setPadding(Ui.dp(c, 2), 0, 0, Ui.dp(c, 4));
            pos.addView(hint);
        }
        n1 = new Forms.Num(c, "x1", work.a1);
        n2 = new Forms.Num(c, "y1", work.a2);
        n3 = new Forms.Num(c, "x2", work.a3);
        n4 = new Forms.Num(c, "y2", work.a4);
        pos.addView(n1.row);
        pos.addView(n2.row);
        pos.addView(n3.row);
        pos.addView(n4.row);

        // ---- 行为 ----
        LinearLayout beh = Forms.section(c, "行为", root);
        Forms.SwitchRow rEn = Forms.switchRow(c, "启用", "停用后核心不识别该区域", work.en != 0, null);
        swEn = rEn.sw;
        beh.addView(rEn.row);
        Forms.SwitchRow rKind = Forms.switchRow(c, "开关型", "点按翻转开/关（可被操作门控）", work.kind == 1, null);
        swKind = rKind.sw;
        beh.addView(rKind.row);

        // ---- 绑定 ----
        LinearLayout bind = Forms.section(c, "绑定（区域被触摸时触发）", root);
        bind.addView(Ui.label(c, "绑定操作（- = 不绑定）"));
        List<String> opts = new ArrayList<String>();
        opts.add("-");
        if (opNames != null) opts.addAll(opNames);
        String sel = opts.contains(work.bindOp) ? work.bindOp : "-";
        bindChips = new Forms.Chips(c, opts, sel, new Runnable() {
            public void run() { applyBind(); }
        });
        bind.addView(bindChips.view);
        evLabel = Ui.label(c, "时机（绑定操作何时触发）");
        bind.addView(evLabel);
        evSeg = new Forms.Seg(c, new String[]{"按下", "完整按压"}, "press".equals(work.bindEv) ? 1 : 0, null);
        bind.addView(evSeg.row);

        applyType();
        applyBind();
    }

    public static RegionEdit build(Context c, Regions.Region work, List<String> opNames, String frameHint) {
        return new RegionEdit(c, work, opNames, frameHint);
    }

    /** 类型切换：矩形 = x1/y1/x2/y2；圆 = 圆心 x/圆心 y/半径 r（y2 隐藏）。 */
    private void applyType() {
        boolean circle = typeSeg.selected() == 1;
        n1.setLabel(circle ? "圆心 x" : "x1");
        n2.setLabel(circle ? "圆心 y" : "y1");
        n3.setLabel(circle ? "半径 r" : "x2");
        n4.setVisible(!circle);
    }

    /** 绑定为“-”时，时机选择不可用（弱化）。 */
    private void applyBind() {
        boolean off = "-".equals(bindChips.selected());
        evLabel.setAlpha(off ? 0.45f : 1f);
        evSeg.row.setAlpha(off ? 0.45f : 1f);
        evSeg.setEnabledAll(!off);
    }

    private Integer intOr(Forms.Num n) {
        try {
            int v = Integer.parseInt(n.field.getText().toString().trim());
            n.clearError();
            return Integer.valueOf(v);
        } catch (Exception e) {
            n.setError("必须是整数（可为负）");
            return null;
        }
    }

    /** 回收表单 → out；基础校验失败时标红并返回错误文案，通过返回 null。 */
    public String collect(Regions.Region out) {
        boolean bad = false;
        out.id = idRow.value();
        if (!Regions.idOk(out.id)) {
            idRow.setError("id 需为 1..15 个可见字符（可用中文），不能是裸 \"-\"");
            bad = true;
        } else {
            idRow.clearError();
        }
        out.type = typeSeg.selected() == 1 ? 1 : 0;

        Integer v1 = intOr(n1);
        Integer v2 = intOr(n2);
        Integer v3 = intOr(n3);
        Integer v4 = intOr(n4);
        if (v1 == null || v2 == null || v3 == null || v4 == null) bad = true;

        out.en = swEn.isChecked() ? 1 : 0;
        out.bindOp = bindChips.selected();
        out.bindEv = evSeg.selected() == 1 ? "press" : "down";
        out.kind = swKind.isChecked() ? 1 : 0;

        if (bad) return "有字段未通过校验（已标红）";
        out.a1 = v1.intValue();
        out.a2 = v2.intValue();
        out.a3 = v3.intValue();
        out.a4 = v4.intValue();
        return null;
    }
}
