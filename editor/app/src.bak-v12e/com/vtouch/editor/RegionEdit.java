package com.vtouch.editor;

import android.content.Context;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 区域编辑页（全屏表单）：分区卡片（基本/位置/行为/绑定）。
 * v12d：位置 = 只读显示 + [在画布上调整位置]——坐标调整全部走可视化画布（全部区域同屏、逐个拖调），
 * 表单不再带数字步进（原 x1/y1/x2/y2 已并入画布）。新建区域请在画布上拖画（区域页 [＋ 新建区域]）。
 */
public final class RegionEdit {
    public final LinearLayout root;

    private final Forms.Text idRow;
    private final Forms.Seg typeSeg;
    private final Switch swEn, swKind;
    private final Forms.Chips bindChips;
    private final TextView evLabel;
    private final Forms.Seg evSeg;

    private RegionEdit(Context c, Regions.Region work, List<String> opNames, String frameHint, Runnable onCanvas) {
        root = Ui.col(c);
        root.setPadding(Ui.dp(c, 12), Ui.dp(c, 0), Ui.dp(c, 12), Ui.dp(c, 12));

        // ---- 基本 ----
        LinearLayout base = Forms.section(c, "基本", root);
        idRow = new Forms.Text(c, "id（字母/数字/下划线/短横，1..15）", work.id, "如 r1");
        base.addView(idRow.row);
        base.addView(Ui.label(c, "类型"));
        typeSeg = new Forms.Seg(c, new String[]{"矩形", "圆"}, work.type == 1 ? 1 : 0, null);
        base.addView(typeSeg.row);

        // ---- 位置（可视化编辑：坐标只在画布上调整） ----
        LinearLayout pos = Forms.section(c, "位置（在画布上可视化调整）", root);
        if (frameHint != null && !frameHint.isEmpty()) {
            TextView hint = Ui.tv(c, frameHint, 11, Ui.DIM, false);
            hint.setPadding(Ui.dp(c, 2), 0, 0, Ui.dp(c, 4));
            pos.addView(hint);
        }
        String geomText = work.type == 1
                ? ("圆 · 圆心 " + work.a1 + "," + work.a2 + " · r=" + work.a3)
                : ("矩形 · " + work.a1 + "," + work.a2 + " → " + work.a3 + "," + work.a4);
        TextView geom = Ui.mono(c, geomText, 13, Ui.TEXT);
        geom.setPadding(Ui.dp(c, 2), Ui.dp(c, 2), 0, Ui.dp(c, 6));
        pos.addView(geom);
        TextView onCanvas2 = Ui.btnSmall(c, "在画布上调整位置 ›");
        onCanvas2.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        if (onCanvas != null) {
            onCanvas2.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { onCanvas.run(); }
            });
        }
        pos.addView(onCanvas2);

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

        applyBind();
    }

    public static RegionEdit build(Context c, Regions.Region work, List<String> opNames, String frameHint, Runnable onCanvas) {
        return new RegionEdit(c, work, opNames, frameHint, onCanvas);
    }

    /** 绑定为“-”时，时机选择不可用（弱化）。 */
    private void applyBind() {
        boolean off = "-".equals(bindChips.selected());
        evLabel.setAlpha(off ? 0.45f : 1f);
        evSeg.row.setAlpha(off ? 0.45f : 1f);
        evSeg.setEnabledAll(!off);
    }

    /** 回收表单 → out（坐标不动：由画布唯一负责）；基础校验失败时标红并返回错误文案，通过返回 null。 */
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
        out.en = swEn.isChecked() ? 1 : 0;
        out.bindOp = bindChips.selected();
        out.bindEv = evSeg.selected() == 1 ? "press" : "down";
        out.kind = swKind.isChecked() ? 1 : 0;
        if (bad) return "有字段未通过校验（已标红）";
        return null;
    }
}
