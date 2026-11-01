package com.vtouch.editor;

import android.content.Context;
import android.content.res.ColorStateList;
import android.os.Handler;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 表单组件库（编辑体验重构）：数字步进行（长按连发+变量选择）、文本行、开关行、
 * 分段选择、胶囊单选、选择对话框。口径：触控 ≥44dp、标签 13sp、值 15sp；
 * 选中态 = 强调色实底；出错 = 行内红色提示 + 输入框红色下划线。
 */
public final class Forms {

    /** 分区卡片：先加分区标题再加卡片，返回卡片内容容器。 */
    public static LinearLayout section(Context c, String title, LinearLayout host) {
        TextView t = Ui.tv(c, title, 13, Ui.DIM, true);
        t.setPadding(Ui.dp(c, 4), Ui.dp(c, 14), 0, Ui.dp(c, 6));
        host.addView(t);
        LinearLayout card = Ui.col(c);
        card.setBackground(Ui.strokedRound(c, Ui.CARD, Ui.CARD_LINE, 8));
        int p = Ui.dp(c, 14);
        card.setPadding(p, Ui.dp(c, 8), p, Ui.dp(c, 10));
        host.addView(card);
        return card;
    }

    // ---------------- 数字步进行 ----------------

    /** 数字步进行：标签行（可挂“变量”按钮）+ [− 输入 +] 步进器（长按连发）+ 行内错误行。 */
    public static final class Num {
        public final LinearLayout row;
        public final EditText field;
        private final Context c;
        private final TextView label;
        private final TextView err;
        private final LinearLayout labelLine;

        public Num(Context c, String text, int value) {
            this.c = c;
            row = Ui.col(c);
            labelLine = Ui.row(c);
            labelLine.setGravity(Gravity.CENTER_VERTICAL);
            label = Ui.tv(c, text, 13, Ui.TEXT, false);
            labelLine.addView(label, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            row.addView(labelLine);
            LinearLayout main = Ui.row(c);
            main.setGravity(Gravity.CENTER_VERTICAL);
            main.setPadding(0, Ui.dp(c, 4), 0, Ui.dp(c, 2));
            main.addView(step("−", -1, this));
            field = new EditText(c);
            field.setHint("0");
            field.setHintTextColor(Ui.DIM);
            field.setTextColor(Ui.TEXT);
            field.setInputType(android.text.InputType.TYPE_CLASS_NUMBER
                    | android.text.InputType.TYPE_NUMBER_FLAG_SIGNED);
            field.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
            field.setPadding(Ui.dp(c, 10), Ui.dp(c, 8), Ui.dp(c, 10), Ui.dp(c, 8));
            field.setText(String.valueOf(value));
            field.setTextSize(15);
            LinearLayout.LayoutParams flp = new LinearLayout.LayoutParams(
                    Ui.dp(c, 118), LinearLayout.LayoutParams.WRAP_CONTENT);
            flp.leftMargin = Ui.dp(c, 6);
            flp.rightMargin = Ui.dp(c, 6);
            main.addView(field, flp);
            main.addView(step("＋", 1, this));
            row.addView(main);
            err = Ui.tv(c, "", 11, Ui.WARN, false);
            err.setVisibility(View.GONE);
            err.setPadding(Ui.dp(c, 2), Ui.dp(c, 2), 0, Ui.dp(c, 2));
            row.addView(err);
        }

        /** 挂“变量”按钮（预置触发变量 + fx/fy）。 */
        public void enableVar() { enableVar(null, null); }

        /** 挂“变量”按钮：预置 7 + 自定义变量（名字 → 编码；v10）填入本行。 */
        public void enableVar(final List<String> extraNames, final List<Integer> extraCodes) {
            TextView b = Ui.tv(c, "变量", 11, Ui.ACCENT, false);
            b.setGravity(Gravity.CENTER);
            b.setBackground(Ui.pressable(c, (Ui.ACCENT & 0x00FFFFFF) | 0x33000000,
                    (Ui.ACCENT & 0x00FFFFFF) | 0x55000000, 6));
            b.setPadding(Ui.dp(c, 10), Ui.dp(c, 7), Ui.dp(c, 10), Ui.dp(c, 7));
            b.setClickable(true);
            b.setFocusable(true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = Ui.dp(c, 8);
            b.setLayoutParams(lp);
            b.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    final List<String> items = new ArrayList<String>();
                    final List<Integer> codes = new ArrayList<Integer>();
                    items.add("按下点 X（tdx）"); codes.add(-1);
                    items.add("按下点 Y（tdy）"); codes.add(-2);
                    items.add("弹起点 X（tux）"); codes.add(-3);
                    items.add("弹起点 Y（tuy）"); codes.add(-4);
                    items.add("按压时长（tms）"); codes.add(-5);
                    items.add("命中点 X（fx）"); codes.add(-6);
                    items.add("命中点 Y（fy）"); codes.add(-7);
                    if (extraNames != null && extraCodes != null) {
                        for (int i = 0; i < extraNames.size(); i++) {
                            items.add("变量 " + extraNames.get(i));
                            codes.add(extraCodes.get(i));
                        }
                    }
                    varDialog(c, items, new Pick() {
                        public void onPick(int index) { setValue(codes.get(index)); clearError(); }
                    });
                }
            });
            labelLine.addView(b);
        }

        /** 挂「取点」按钮（看图器取点回填；坐标字段用）。 */
        public void enablePick(final Runnable onPick) {
            enablePick("取点", onPick);
        }

        public void enablePick(String label, final Runnable onPick) {
            TextView b = Ui.tv(c, label, 11, Ui.ACCENT, false);
            b.setGravity(Gravity.CENTER);
            b.setBackground(Ui.pressable(c, (Ui.ACCENT & 0x00FFFFFF) | 0x33000000,
                    (Ui.ACCENT & 0x00FFFFFF) | 0x55000000, 6));
            b.setPadding(Ui.dp(c, 10), Ui.dp(c, 7), Ui.dp(c, 10), Ui.dp(c, 7));
            b.setClickable(true);
            b.setFocusable(true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = Ui.dp(c, 8);
            b.setLayoutParams(lp);
            b.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { if (onPick != null) onPick.run(); }
            });
            labelLine.addView(b);
        }

        public int value() {
            return Integer.parseInt(field.getText().toString().trim());
        }

        public void setValue(int v) { field.setText(String.valueOf(v)); }

        public void setLabel(String s) { label.setText(s); }

        public void setVisible(boolean v) { row.setVisibility(v ? View.VISIBLE : View.GONE); }

        public void setError(String s) {
            field.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.WARN, 6));
            err.setText(s);
            err.setVisibility(View.VISIBLE);
        }

        public void clearError() {
            field.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
            err.setVisibility(View.GONE);
        }

        private TextView step(String s, final int dir, final Num owner) {
            final Context cc = c;
            final TextView b = Ui.tv(cc, s, 20, Ui.TEXT, false);
            b.setGravity(Gravity.CENTER);
            b.setBackground(Ui.pressable(cc, Ui.CARD2, 0xFF2A3138, 6));
            b.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(cc, 44), Ui.dp(cc, 46)));
            b.setFocusable(false);
            b.setOnTouchListener(new View.OnTouchListener() {
                private final Handler h = new Handler();
                private boolean longFired = false;
                private final Runnable rep = new Runnable() {
                    public void run() {
                        if (!longFired) {
                            // 长按连发首次触发：给一次触感反馈（未挂载窗口时为 no-op）
                            b.performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
                        }
                        longFired = true;
                        bump(owner, dir, 10);      // 连发：步进 10，快速大范围调整
                        h.postDelayed(this, 80);
                    }
                };

                public boolean onTouch(View v, MotionEvent e) {
                    int a = e.getActionMasked();
                    if (a == MotionEvent.ACTION_DOWN) {
                        longFired = false;
                        v.setAlpha(0.55f);
                        h.postDelayed(rep, 350);
                    } else if (a == MotionEvent.ACTION_UP) {
                        h.removeCallbacks(rep);
                        v.setAlpha(1f);
                        if (!longFired) bump(owner, dir, 1);
                    } else if (a == MotionEvent.ACTION_CANCEL) {
                        h.removeCallbacks(rep);
                        v.setAlpha(1f);
                    }
                    return true;
                }
            });
            return b;
        }

        private static void bump(Num owner, int dir, int step) {
            int v;
            try { v = Integer.parseInt(owner.field.getText().toString().trim()); }
            catch (Exception e) { v = 0; }
            owner.setValue(v + dir * step);
            owner.clearError();
        }
    }

    // ---------------- 文本行 ----------------

    public static final class Text {
        public final LinearLayout row;
        public final EditText field;
        private final TextView err;
        private final Context c;

        public Text(Context c, String label, String value, String hint) {
            this.c = c;
            row = Ui.col(c);
            row.addView(Ui.label(c, label));
            field = Ui.textField(c, hint);
            field.setText(value == null ? "" : value);
            field.setTextSize(15);
            row.addView(field);
            err = Ui.tv(c, "", 11, Ui.WARN, false);
            err.setVisibility(View.GONE);
            err.setPadding(Ui.dp(c, 2), Ui.dp(c, 2), 0, Ui.dp(c, 2));
            row.addView(err);
        }

        public String value() { return field.getText().toString().trim(); }

        public void setError(String s) {
            field.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.WARN, 6));
            err.setText(s);
            err.setVisibility(View.VISIBLE);
        }

        public void clearError() {
            field.setBackground(Ui.strokedRound(c, Ui.CARD2, Ui.CARD_LINE, 6));
            err.setVisibility(View.GONE);
        }
    }

    // ---------------- 开关行 ----------------

    public static final class SwitchRow {
        public final LinearLayout row;
        public final Switch sw;

        public SwitchRow(LinearLayout r, Switch s) { row = r; sw = s; }
    }

    /** 开关行：标题 + 说明 + 右侧 Switch；整行可点（热区 52dp+），开关启用着色。 */
    public static SwitchRow switchRow(Context c, String title, String desc, boolean on,
                                      final CompoundButton.OnCheckedChangeListener cb) {
        LinearLayout row = Ui.row(c);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(Ui.dp(c, 52));
        row.setPadding(0, Ui.dp(c, 4), 0, Ui.dp(c, 4));
        LinearLayout col = Ui.col(c);
        col.addView(Ui.tv(c, title, 14, Ui.TEXT, false));
        if (desc != null) {
            TextView d = Ui.tv(c, desc, 11, Ui.DIM, false);
            d.setPadding(0, Ui.dp(c, 2), 0, 0);
            col.addView(d);
        }
        final Switch sw = new Switch(c);
        // 着色：开 = 强调色；关 = 灰（暗色下状态一眼可辨）
        int[][] states = new int[][]{
                new int[]{android.R.attr.state_checked},
                new int[]{},
        };
        sw.setThumbTintList(new ColorStateList(states, new int[]{Ui.ACCENT, Ui.FAINT}));
        sw.setTrackTintList(new ColorStateList(states, new int[]{0x7AE8B14F, 0x4D2A3037}));
        sw.setChecked(on);
        if (cb != null) sw.setOnCheckedChangeListener(cb);
        row.addView(col, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(sw);
        row.setClickable(true);
        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { sw.toggle(); }
        });
        return new SwitchRow(row, sw);
    }

    // ---------------- 分段选择（一排等宽胶囊） ----------------

    public static final class Seg {
        public final LinearLayout row;
        private final TextView[] items;
        private int selected;

        public Seg(Context c, String[] opts, int sel, final Runnable onPick) {
            row = Ui.row(c);
            row.setGravity(Gravity.CENTER_VERTICAL);
            selected = sel;
            items = new TextView[opts.length];
            for (int i = 0; i < opts.length; i++) {
                final int idx = i;
                TextView t = Ui.tv(c, opts[i], 13, Ui.DIM, false);
                t.setGravity(Gravity.CENTER);
                t.setPadding(0, Ui.dp(c, 13), 0, Ui.dp(c, 13));
                t.setClickable(true);
                t.setFocusable(true);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                        0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
                if (i > 0) lp.leftMargin = Ui.dp(c, 6);
                t.setLayoutParams(lp);
                t.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        pick(idx);
                        if (onPick != null) onPick.run();
                    }
                });
                items[i] = t;
                row.addView(t);
            }
            paint();
        }

        private void paint() {
            for (int i = 0; i < items.length; i++) {
                boolean on = i == selected;
                items[i].setTextColor(on ? Ui.ACCENT : Ui.DIM);
                items[i].setTypeface(items[i].getTypeface(),
                        on ? android.graphics.Typeface.BOLD : android.graphics.Typeface.NORMAL);
                items[i].setBackground(on
                        ? Ui.strokedRound(items[i].getContext(), (Ui.ACCENT & 0x00FFFFFF) | 0x33000000, (Ui.ACCENT & 0x00FFFFFF) | 0x59000000, 6)
                        : Ui.strokedRound(items[i].getContext(), 0x00000000, Ui.CARD_LINE, 6));
            }
        }

        public void pick(int i) { selected = i; paint(); }

        /** v13：程序化置选中（不触发 onPick）。 */
        public void setSelectedSilent(int i) {
            if (i < 0 || i >= items.length) return;
            selected = i;
            paint();
        }

        public int selected() { return selected; }

        public void setEnabledAll(boolean en) {
            for (TextView t : items) {
                t.setEnabled(en);
                t.setAlpha(en ? 1f : 0.62f);
            }
        }
    }

    // ---------------- 胶囊单选（横向滚动，项目多时用） ----------------

    public static final class Chips {
        public final HorizontalScrollView view;
        private final String[] opts;
        private final TextView[] items;
        private String selected;

        public Chips(Context c, List<String> options, String sel, final Runnable onPick) {
            opts = options.toArray(new String[0]);
            selected = sel;
            LinearLayout strip = Ui.row(c);
            strip.setPadding(0, Ui.dp(c, 2), Ui.dp(c, 6), Ui.dp(c, 2));
            items = new TextView[opts.length];
            for (int i = 0; i < opts.length; i++) {
                final int idx = i;
                TextView t = Ui.tv(c, opts[i], 13, Ui.DIM, false);
                t.setGravity(Gravity.CENTER);
                t.setPadding(Ui.dp(c, 14), Ui.dp(c, 9), Ui.dp(c, 14), Ui.dp(c, 9));
                t.setClickable(true);
                t.setFocusable(true);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
                lp.rightMargin = Ui.dp(c, 6);
                t.setLayoutParams(lp);
                t.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        pick(idx);
                        if (onPick != null) onPick.run();
                    }
                });
                items[i] = t;
                strip.addView(t);
            }
            view = new HorizontalScrollView(c);
            view.setHorizontalScrollBarEnabled(false);
            view.setFillViewport(false);
            view.addView(strip);
            paint();
        }

        private void paint() {
            for (int i = 0; i < items.length; i++) {
                boolean on = opts[i].equals(selected);
                items[i].setTextColor(on ? Ui.ACCENT : Ui.DIM);
                items[i].setBackground(on
                        ? Ui.strokedRound(items[i].getContext(), (Ui.ACCENT & 0x00FFFFFF) | 0x33000000, (Ui.ACCENT & 0x00FFFFFF) | 0x59000000, 6)
                        : Ui.strokedRound(items[i].getContext(), 0x00000000, Ui.CARD_LINE, 6));
            }
        }

        public void pick(int i) { selected = opts[i]; paint(); }

        public String selected() { return selected; }

        public void setSelected(String s) { selected = s; paint(); }
    }

    // ---------------- 选择对话框 ----------------

    public interface Pick { void onPick(int index); }

    /** 选择列表对话框（自绘样式；替代系统下拉/单选）。 */
    public static Ui.Dlg pickDialog(Context c, String title, List<String> items, int current, final Pick onPick) {
        Ui.Dlg d = pickDialogBuild(c, title, items, current, onPick);
        d.show();
        return d;
    }

    /** 只构建不显示（供离屏预览与复用）。 */
    public static Ui.Dlg pickDialogBuild(Context c, String title, List<String> items, int current, final Pick onPick) {
        final Ui.Dlg d = Ui.dialog(c, title);
        LinearLayout col = Ui.col(c);
        for (int i = 0; i < items.size(); i++) {
            final int idx = i;
            TextView t = Ui.tv(c, items.get(i), 15,
                    i == current ? Ui.ACCENT : Ui.TEXT, i == current);
            t.setPadding(Ui.dp(c, 12), Ui.dp(c, 14), Ui.dp(c, 12), Ui.dp(c, 14));
            t.setBackground(Ui.pressable(c, 0x00000000, 0x40FFFFFF, 6));
            t.setClickable(true);
            t.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    d.dismiss();
                    if (onPick != null) onPick.onPick(idx);
                }
            });
            col.addView(t);
        }
        d.body.addView(col);
        TextView cancel = Ui.btnFlat(c, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        d.actions.addView(cancel);
        return d;
    }

    /** 变量选择对话框（人话入口；条目由调用方给：预置 + 自定义）。 */
    public static void varDialog(Context c, List<String> items, final Pick onPick) {
        pickDialog(c, "使用变量", items, -1, onPick);
    }

    /** 类型选择回调（v12.1）。 */
    public interface TypePick { void onPick(int type); }

    /** 步骤类型底部弹层（v12.1：「添加步骤」与步骤编辑页换类型共用；名称 + 一句说明 + 轻分组）。 */
    public static Ui.Dlg typeSheet(Context c, final TypePick onPick) {
        final Ui.Dlg d = Ui.dialog(c, "添加步骤");
        LinearLayout col = Ui.col(c);
        final String[][] groups = {
                {"基础", "点按|在指定位置点一下（可设按住时长）", "滑动|从起点滑到终点", "等待|什么都不做，等一段时间",
                 "按下|按住不放（配合弹起使用）", "弹起|松开按住的手指"},
                {"判断与视觉", "区域判断|判断某坐标是否在区域内（带两分支）", "开关判断|判断开关型区域是否开着（带两分支）",
                 "跳转|跳到第 N 步 / 结束（旧式循环写法）", "计算|算一个值存进变量",
                 "找图|在屏幕上找图片（命中坐标可给点按用）", "找色|在屏幕上找颜色（单点 / 多点）"},
                {"流程控制", "循环|重复一段步骤（次数 / 时长 / 无限 / 到条件）", "条件块|如果…否则…（自动带结束标记）",
                 "跳出循环|立即跳出当前循环", "继续循环|跳到当前循环的下一轮"},
        };
        for (String[] g : groups) {
            TextView h = Ui.tv(c, g[0], 11, Ui.FAINT, false);
            h.setPadding(Ui.dp(c, 12), Ui.dp(c, 14), Ui.dp(c, 12), Ui.dp(c, 4));
            col.addView(h);
            for (int i = 1; i < g.length; i++) {
                String[] kv = g[i].split("\\|");
                final String name = kv[0];
                LinearLayout row = Ui.col(c);
                row.setPadding(Ui.dp(c, 12), Ui.dp(c, 8), Ui.dp(c, 12), Ui.dp(c, 8));
                row.setBackground(Ui.pressable(c, 0x00000000, 0x40FFFFFF, 6));
                row.setClickable(true);
                row.addView(Ui.tv(c, name, 14.5f, Ui.TEXT, true));
                TextView desc = Ui.tv(c, kv[1], 11.5f, Ui.DIM, false);
                desc.setPadding(0, Ui.dp(c, 2), 0, 0);
                row.addView(desc);
                row.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        d.dismiss();
                        if (onPick != null) onPick.onPick(StepEdit.typeIdByName(name));
                    }
                });
                col.addView(row);
            }
        }
        android.widget.ScrollView sv = new android.widget.ScrollView(c);
        sv.setVerticalScrollBarEnabled(false);
        sv.addView(col);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Ui.dp(c, 430));
        sv.setLayoutParams(slp);
        d.body.addView(sv);
        TextView cancel = Ui.btnFlat(c, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        d.actions.addView(cancel);
        return d;
    }
}
