package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.List;

/**
 * 步骤流程总览（紧凑时间线）：编号节点 + 连线 + 类型/摘要；控制流步骤（判断/跳转）用强调色标出。
 * 点任意行 = 进入该步骤编辑。供操作编辑页「流程总览」模式使用，也可离屏渲染验收。
 */
public final class StepFlow {
    public interface Tap { void onTap(int idx); }

    public static LinearLayout build(Context c, final List<Ops.Step> steps, final Tap onTap) {
        LinearLayout card = Ui.card(c);
        card.setPadding(Ui.dp(c, 14), Ui.dp(c, 10), Ui.dp(c, 14), Ui.dp(c, 10));
        if (steps.isEmpty()) {
            card.addView(Ui.tv(c, "还没有步骤（用下方「＋ 添加步骤」添加）", 13, Ui.DIM, false));
            return card;
        }
        for (int i = 0; i < steps.size(); i++) {
            final int idx = i;
            final Ops.Step s = steps.get(i);
            LinearLayout row = Ui.row(c);

            // 左列：时间线（上连线 / 编号圆 / 下连线；首尾段隐藏）
            LinearLayout rail = Ui.col(c);
            rail.setGravity(Gravity.CENTER_HORIZONTAL);
            rail.setLayoutParams(new LinearLayout.LayoutParams(
                    Ui.dp(c, 26), LinearLayout.LayoutParams.MATCH_PARENT));
            View topSeg = new View(c);
            topSeg.setBackgroundColor(Ui.CARD_LINE);
            rail.addView(topSeg, new LinearLayout.LayoutParams(Ui.dp(c, 2), 0, 1f));
            TextView dot = Ui.tv(c, String.valueOf(i + 1), 11, Ui.DIM, true);
            dot.setGravity(Gravity.CENTER);
            dot.setBackground(Ui.round(c, Ui.CARD2, 999));
            LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(Ui.dp(c, 22), Ui.dp(c, 22));
            dlp.topMargin = Ui.dp(c, 2);
            dlp.bottomMargin = Ui.dp(c, 2);
            dot.setLayoutParams(dlp);
            rail.addView(dot);
            View botSeg = new View(c);
            botSeg.setBackgroundColor(Ui.CARD_LINE);
            rail.addView(botSeg, new LinearLayout.LayoutParams(Ui.dp(c, 2), 0, 1f));
            if (i == 0) topSeg.setVisibility(View.INVISIBLE);
            if (i == steps.size() - 1) botSeg.setVisibility(View.INVISIBLE);
            row.addView(rail);

            // 右内容：类型 + 摘要（判断/跳转 = 强调色，便于一眼看出控制流）
            LinearLayout body = Ui.col(c);
            body.setPadding(Ui.dp(c, 10), Ui.dp(c, 6), 0, Ui.dp(c, 6));
            LinearLayout titleLine = Ui.row(c);
            titleLine.setGravity(Gravity.CENTER_VERTICAL);
            boolean ctrl = s.type == Ops.T_COND_REGION || s.type == Ops.T_COND_TOGGLE
                    || s.type == Ops.T_JUMP;
            titleLine.addView(Ui.tv(c, Ops.typeName(s.type), 13.5f, ctrl ? Ui.ACCENT : Ui.TEXT, true));
            if (!s.known()) titleLine.addView(Ui.chip(c, "未解锁", Ui.WARN));
            body.addView(titleLine);
            TextView sum = Ui.tv(c, s.summary(), 12, Ui.DIM, false);
            sum.setPadding(0, Ui.dp(c, 2), 0, 0);
            body.addView(sum);
            row.addView(body, new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

            if (s.known() && onTap != null) {
                row.setBackground(Ui.pressable(c, 0x00000000, 0x30FFFFFF, 10));
                row.setClickable(true);
                row.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { onTap.onTap(idx); }
                });
            }
            card.addView(row);
        }
        return card;
    }
}
