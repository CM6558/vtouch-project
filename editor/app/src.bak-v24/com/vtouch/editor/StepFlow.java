package com.vtouch.editor;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.List;
import java.util.Set;

/**
 * 步骤流程总览（紧凑时间线）：编号节点 + 连线 + 类型/摘要；控制流步骤（判断/跳转）用强调色标出。
 * 点任意行 = 进入该步骤编辑。供操作编辑页「流程总览」模式使用，也可离屏渲染验收。
 * v12.3：代码块标注——缩进指示线（每层一色、块尾连线到底）+ 块头底色（贴合色带）+ 块头 ▼/▸ 展开收起。
 */
public final class StepFlow {
    public interface Tap { void onTap(int idx); }

    /** 块头展开/收起回调（v12.3；headIdx = 该块头步骤下标）。 */
    public interface Toggle { void onToggle(int headIdx); }

    /** 缩进指示线（v12.3）：在 14dp 格内 x=8dp 处画 2dp 竖线；行高自适应。 */
    static final class GuideLine extends Drawable {
        private final Paint p = new Paint();
        private final float x, wd;

        GuideLine(Context c, int color) {
            p.setColor(color);
            x = Ui.dp(c, 8);
            wd = Ui.dp(c, 2);
        }

        @Override public void draw(Canvas canvas) {
            Rect b = getBounds();
            canvas.drawRect(b.left + x, b.top, b.left + x + wd, b.bottom, p);
        }

        @Override public void setAlpha(int a) { p.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter cf) { p.setColorFilter(cf); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    public static LinearLayout build(Context c, final List<Ops.Step> steps, final Tap onTap,
                                     final Set<Ops.Step> collapsed, final Toggle onToggle) {
        LinearLayout card = Ui.card(c);
        card.setPadding(Ui.dp(c, 14), Ui.dp(c, 10), Ui.dp(c, 14), Ui.dp(c, 10));
        if (steps.isEmpty()) {
            card.addView(Ui.tv(c, "还没有步骤（用下方「＋ 添加步骤」添加）", 13, Ui.DIM, false));
            return card;
        }
        int n = steps.size();
        int[] dep = depthsOf(steps);     // v6B：唯一缩进口径（否则/子条件=块头层；块内 +1；块尾=块头）
        int[] barCol = new int[n];       // 块内行左色条（0 = 无；色 = 最近块色）
        int[][] guides = new int[n][];   // v12.3：每行缩进指示线颜色（从外到内逐层）
        java.util.ArrayList<Integer> colStk = new java.util.ArrayList<Integer>();
        for (int i = 0; i < n; i++) {
            int t = steps.get(i).type;
            boolean close = (t == Ops.T_ENDLOOP || t == Ops.T_ENDIF);
            boolean atLevel = (t == Ops.T_ELSE || t == Ops.T_CLAUSE);   // v6B：与块头同级（`} else {` 口径）
            int lane = dep[i];
            if (close) {
                int own = colStk.isEmpty() ? 0 : colStk.remove(colStk.size() - 1);   // 块尾：自身块色连线到底
                guides[i] = new int[lane + 1];
                for (int k = 0; k < lane && k < colStk.size(); k++) guides[i][k] = colStk.get(k);
                guides[i][lane] = own;
                barCol[i] = (lane > 0 && !colStk.isEmpty()) ? colStk.get(Math.min(lane, colStk.size()) - 1) : 0;
            } else if (atLevel) {
                guides[i] = new int[lane];
                for (int k = 0; k < lane && k < colStk.size(); k++) guides[i][k] = colStk.get(k);
                barCol[i] = colStk.isEmpty() ? 0 : colStk.get(colStk.size() - 1);    // 本块色
            } else {
                guides[i] = new int[lane];
                for (int k = 0; k < lane && k < colStk.size(); k++) guides[i][k] = colStk.get(k);
                barCol[i] = (lane > 0 && !colStk.isEmpty()) ? colStk.get(Math.min(lane, colStk.size()) - 1) : 0;
                if (t == Ops.T_LOOP || t == Ops.T_IF) colStk.add(t == Ops.T_LOOP ? Ui.ACCENT : Ui.COND_BAR);
            }
        }
        for (int i = 0; i < n; i++) {
            final int idx = i;
            final Ops.Step s = steps.get(i);
            boolean head = s.type == Ops.T_LOOP || s.type == Ops.T_IF;
            int foldTail = -1;
            if (head && collapsed != null && collapsed.contains(s)) {
                int tt = Ops.pairTail(steps, i);
                if (tt > i) foldTail = tt;               // 配对失败按展开处理
            }
            boolean folded = foldTail > i;

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

            // v12.3：缩进指示线带（每层一色；行高铺满——上下行相接成连续竖线，块尾连线到底）
            if (guides[i].length > 0) {
                LinearLayout gstrip = Ui.row(c);
                for (int gi = 0; gi < guides[i].length; gi++) {
                    View gv = new View(c);
                    gv.setBackground(new GuideLine(c, (guides[i][gi] & 0x00FFFFFF) | 0x66000000));
                    gstrip.addView(gv, new LinearLayout.LayoutParams(
                            Ui.dp(c, 14), LinearLayout.LayoutParams.MATCH_PARENT));
                }
                row.addView(gstrip, new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.MATCH_PARENT));
            }

            // 右内容：类型 + 摘要（控制流 = 强调色；块尾/否则 = 灰；块内行带左色条）
            LinearLayout body = Ui.col(c);
            body.setPadding(Ui.dp(c, 10), Ui.dp(c, 6), 0, Ui.dp(c, 6));
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
            if (s.type == Ops.T_CLAUSE) {
                titleLine.addView(Ui.tv(c, s.clauseSummary(Ops.varSlotsOf(steps)), 12.5f, Ui.DIM, false));   // v6B：子条件行（单行）
            } else {
                titleLine.addView(Ui.tv(c, Ops.typeName(s.type), 13.5f,
                        tail ? Ui.FAINT : (ctrl ? Ui.ACCENT : Ui.TEXT), true));
            }
            if (folded) titleLine.addView(Ui.chip(c, "已收起 " + (foldTail - i - 1) + " 步", Ui.DIM));
            if (!s.known()) titleLine.addView(Ui.chip(c, "未解锁", Ui.WARN));
            body.addView(titleLine);
            if (s.type != Ops.T_CLAUSE) {
                TextView sum = Ui.tv(c, s.summary(Ops.varSlotsOf(steps)), 12, Ui.DIM, false);
                sum.setPadding(0, Ui.dp(c, 2), 0, 0);
                body.addView(sum);
            }
            String br = branchLine(idx, steps);
            if (br != null) {
                TextView bt = Ui.tv(c, br, 11.5f, Ui.DIM, false);
                bt.setPadding(0, Ui.dp(c, 3), 0, 0);
                body.addView(bt);
            }
            row.addView(body, new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

            // v12.3：块头 ▼/▸ 展开收起（右侧，40dp 热区）；行其余部分点击 = 进入编辑
            if (head) {
                final TextView chev = Ui.tv(c, folded ? "▸" : "▼", 13, Ui.DIM, false);
                chev.setGravity(Gravity.CENTER);
                chev.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(c, 40), Ui.dp(c, 40)));
                chev.setBackground(Ui.pressable(c, 0x00000000, 0x30FFFFFF, 8));
                chev.setClickable(true);
                chev.setFocusable(true);
                if (onToggle != null) {
                    chev.setOnClickListener(new View.OnClickListener() {
                        public void onClick(View v) { onToggle.onToggle(idx); }
                    });
                }
                row.addView(chev);
            }

            int bg = head ? (s.type == Ops.T_LOOP ? Ui.LOOP_TINT : Ui.COND_TINT) : 0x00000000;
            if (s.known() && onTap != null) {
                row.setBackground(Ui.pressable(c, bg, 0x30FFFFFF, 10));
                row.setClickable(true);
                row.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { onTap.onTap(idx); }
                });
            } else if (head) {
                row.setBackground(Ui.round(c, bg, 10));   // 无交互渲染也标块头底色
            }
            card.addView(row);

            if (folded) i = foldTail - 1;   // 跳过块体（for 自增后正好落在块尾行）
        }
        return card;
    }

    /** 每步的缩进深度（与渲染共用）：块内行 = 块头 +1；块尾 = 块头同级；
     *  v12.2：条件块「否则」与「如果」同级（`} else {` 与 `if` 对齐的代码格式化口径）；
     *  v6B：子条件行（19）同「否则」——与块头同级。 */
    static int[] depthsOf(List<Ops.Step> steps) {
        int n = steps.size();
        int[] dep = new int[n];
        int d = 0;
        for (int i = 0; i < n; i++) {
            int t = steps.get(i).type;
            if (t == Ops.T_ENDLOOP || t == Ops.T_ENDIF) { d--; if (d < 0) d = 0; }
            dep[i] = d;
            if ((t == Ops.T_ELSE || t == Ops.T_CLAUSE) && dep[i] > 0) dep[i] -= 1;
            if (t == Ops.T_LOOP || t == Ops.T_IF) d++;
        }
        return dep;
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
