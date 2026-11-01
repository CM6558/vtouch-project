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
        int n = steps.size();
        int[] dep = new int[n];          // v12 块树：嵌套深度（缩进）
        int[] barCol = new int[n];       // 块内行左色条（0 = 无；色 = 最近块色）
        int d = 0;
        int[] cstk = new int[16];
        for (int i = 0; i < n; i++) {
            int t = steps.get(i).type;
            if (t == Ops.T_ENDLOOP || t == Ops.T_ENDIF) { d--; if (d < 0) d = 0; }
            dep[i] = d;
            barCol[i] = (d > 0) ? cstk[d - 1] : 0;
            if (t == Ops.T_LOOP || t == Ops.T_IF) {
                if (d < 16) cstk[d] = (t == Ops.T_LOOP) ? Ui.ACCENT : Ui.COND_BAR;
                d++;
            }
        }
        for (int i = 0; i < n; i++) {
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
            TextView dot = Ui.monoBold(c, String.valueOf(i + 1), 11, Ui.DIM);
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

            // 右内容：类型 + 摘要（控制流 = 强调色；块尾/否则 = 灰；块内行缩进 + 左色条）
            LinearLayout body = Ui.col(c);
            body.setPadding(Ui.dp(c, 10 + dep[i] * 14), Ui.dp(c, 6), 0, Ui.dp(c, 6));
            LinearLayout titleLine = Ui.row(c);
            titleLine.setGravity(Gravity.CENTER_VERTICAL);
            if (barCol[i] != 0) {
                View bar = new View(c);
                bar.setBackgroundColor(barCol[i]);
                LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(Ui.dp(c, 3), Ui.dp(c, 14));
                blp.rightMargin = Ui.dp(c, 6);
                bar.setLayoutParams(blp);
                titleLine.addView(bar);
            }
            boolean ctrl = s.type == Ops.T_COND_REGION || s.type == Ops.T_COND_TOGGLE || s.type == Ops.T_JUMP
                    || s.type == Ops.T_LOOP || s.type == Ops.T_IF || s.type == Ops.T_BREAK || s.type == Ops.T_CONT;
            boolean tail = s.type == Ops.T_ENDLOOP || s.type == Ops.T_ENDIF || s.type == Ops.T_ELSE;
            titleLine.addView(Ui.tv(c, Ops.typeName(s.type), 13.5f,
                    tail ? Ui.FAINT : (ctrl ? Ui.ACCENT : Ui.TEXT), true));
            if (!s.known()) titleLine.addView(Ui.chip(c, "未解锁", Ui.WARN));
            body.addView(titleLine);
            TextView sum = Ui.tv(c, s.summary(), 12, Ui.DIM, false);
            sum.setPadding(0, Ui.dp(c, 2), 0, 0);
            body.addView(sum);
            String br = branchLine(idx, steps);
            if (br != null) {
                TextView bt = Ui.tv(c, br, 11.5f, Ui.DIM, false);
                bt.setPadding(0, Ui.dp(c, 3), 0, 0);
                body.addView(bt);
            }
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

    /** 分支走向一行小字（v12.1：条件/视觉/跳转步——目标带步骤名提示；其余返回 null）。 */
    private static String branchLine(int idx, List<Ops.Step> steps) {
        Ops.Step s = steps.get(idx);
        if (s.type == Ops.T_COND_REGION || s.type == Ops.T_COND_TOGGLE
                || s.type == Ops.T_FINDIMAGE || s.type == Ops.T_FINDCOLOR) {
            return "↳ 成立 → " + sideLabel(s.a4, s.j1, steps) + "　｜　不成立 → " + sideLabel(s.a3, s.j2, steps);
        }
        if (s.type == Ops.T_JUMP) {
            return "↳ → " + targetLabel(s.a1, steps);
        }
        return null;
    }

    private static String sideLabel(int tier, int target, List<Ops.Step> steps) {
        if (tier == Ops.C_JUMP) return targetLabel(target, steps);
        return Ops.tierName(tier);
    }

    private static String targetLabel(int target, List<Ops.Step> steps) {
        if (target == 0) return "结束";
        if (target < 1 || target > steps.size()) return "第 " + target + " 步";
        Ops.Step t = steps.get(target - 1);
        return "第 " + target + " 步「" + Ops.typeName(t.type) + "」";
    }
}
