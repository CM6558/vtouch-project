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
        // v32：变量槽（编码 ⇄ 显示名映射；「变量」选定后框内显名不显编码）
        private List<String> xNames;
        private List<Integer> xCodes;

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

        /** 挂“变量”按钮：预置 7 + 自定义变量（名字 → 编码；v10）填入本行。
         *  v32：选定后框内显示变量名（如 fx / dx），读回时名字自动解析回编码；按钮改轻量描边小键。 */
        public void enableVar(final List<String> extraNames, final List<Integer> extraCodes) {
            xNames = extraNames;
            xCodes = extraCodes;
            TextView b = Ui.btnAux(c, "变量", Ui.ACCENT);
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
            refreshVarDisplay();                    // 初值本就是变量编码（如 -6）→ 立即显名
        }

        /** 挂「取点」按钮（看图器取点回填；坐标字段用）。 */
        public void enablePick(final Runnable onPick) {
            enablePick("取点", onPick);
        }

        public void enablePick(String label, final Runnable onPick) {
            TextView b = Ui.btnAux(c, label, Ui.ACCENT);      // v32：轻量描边小键（原实底盒子）
            b.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { if (onPick != null) onPick.run(); }
            });
            labelLine.addView(b);
        }

        /** v32：解析框内文本——整数直接返回；否则按变量名解析（tdx..fy / 自定义槽名）；都不行抛异常（调用方标红）。 */
        public int value() {
            String s = field.getText().toString().trim();
            try {
                return Integer.parseInt(s);
            } catch (Exception ig) { }
            Integer code = resolveName(s);
            if (code != null) return code;
            throw new NumberFormatException("非整数且非已知变量名: " + s);
        }

        /** v32：负值（变量编码）→ 显示变量名（如 fx / dx）；无名字对照时保持数字。 */
        public void setValue(int v) {
            String n = (v < 0) ? nameOf(v) : null;
            field.setText(n != null ? n : String.valueOf(v));
        }

        /** v32：编码 → 显示名（预置 tdx..fy / 自定义槽名；无对照 = null）。 */
        private String nameOf(int code) {
            switch (code) {
                case -1: return "tdx"; case -2: return "tdy"; case -3: return "tux"; case -4: return "tuy";
                case -5: return "tms"; case -6: return "fx"; case -7: return "fy";
                default: break;
            }
            if (xCodes != null && xNames != null) {
                for (int i = 0; i < xCodes.size() && i < xNames.size(); i++) {
                    if (xCodes.get(i) == code && xNames.get(i) != null) return xNames.get(i);
                }
            }
            return null;
        }

        /** v32：名字 → 编码（预置或自定义槽名；解析不中 = null）。 */
        private Integer resolveName(String s) {
            if (s == null || s.isEmpty()) return null;
            switch (s) {
                case "tdx": return -1; case "tdy": return -2; case "tux": return -3; case "tuy": return -4;
                case "tms": return -5; case "fx": return -6; case "fy": return -7;
                default: break;
            }
            if (xNames != null && xCodes != null) {
                for (int i = 0; i < xNames.size() && i < xCodes.size(); i++) {
                    if (s.equals(xNames.get(i))) return xCodes.get(i);
                }
            }
            return null;
        }

        /** v32：框内初值若为变量编码（负）→ 换成显示名（enableVar 挂完后调用）。 */
        private void refreshVarDisplay() {
            String s = field.getText().toString().trim();
            try {
                int v = Integer.parseInt(s);
                if (v < 0) {
                    String n = nameOf(v);
                    if (n != null) field.setText(n);
                }
            } catch (Exception ig) { }
        }

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
            final TextView b = Ui.tv(cc, s, 18, Ui.TEXT, false);
            b.setGravity(Gravity.CENTER);
            b.setBackground(Ui.pressable(cc, Ui.CARD2, 0xFF2A3138, 6));
            b.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(cc, 40), Ui.dp(cc, 40)));
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
            String s = owner.field.getText().toString().trim();
            try {
                v = Integer.parseInt(s);
            } catch (Exception e) {
                if (!s.isEmpty() && owner.resolveName(s) != null) return;   // v32：变量名不参与 ± 步进（换变量走「变量」键）
                v = 0;
            }
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
                TextView t = Ui.tv(c, opts[i], 12, Ui.DIM, false);
                t.setGravity(Gravity.CENTER);
                // v32：紧凑分段（单行防「区域判/断」断字）。⚠ 勿用 setSingleLine——离屏渲染下该调用会让
                // 整格文字不绘制（seg 探针 V0/V4 实锤 txt=0；V1/V3 正常），maxLines 走另一条布局路径，渲染正常
                t.setMaxLines(1);
                t.setMinHeight(Ui.dp(c, 40));                              // 触控底线 40dp；选中靠着色表达，不再给每项套盒子
                t.setClickable(true);
                t.setFocusable(true);
                // v32：按标签字数分配权重——等宽格在窄容器（子条件卡内 ~185px/4字格）放不下 4 字标签，
                // 曾把「断」裁掉；len+1 实测最紧余量 +12px（len+4 时余量 -9px，仍裁，见 UX_BATCH_v32 体检核）
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                        0, LinearLayout.LayoutParams.WRAP_CONTENT, opts[i].length() + 1f);
                if (i > 0) lp.leftMargin = Ui.dp(c, 6);
                t.setPadding(Ui.dp(c, 4), 0, Ui.dp(c, 4), 0);
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
                // v32：选中 = 琥珀浅底 + 琥珀粗字；未选 = 纯净文字键（原来「项项带描边盒子」= 臃肿源）
                items[i].setBackground(on
                        ? Ui.round(items[i].getContext(), (Ui.ACCENT & 0x00FFFFFF) | 0x2E000000, 8)
                        : Ui.pressable(items[i].getContext(), 0x00000000, 0x40FFFFFF, 8));
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

        /** v32：体检用——格数 / 第 i 格（防「区域判断」尾字被裁的几何断言用）。 */
        public int itemsCountForTest() { return items.length; }
        public TextView itemForTest(int i) { return items[i]; }
    }

    // ---------------- 胶囊单选（横向滚动，项目多时用） ----------------

    public static final class Chips {
        public final HorizontalScrollView view;
        private final List<String> opts = new java.util.ArrayList<String>();
        private final List<TextView> items = new java.util.ArrayList<TextView>();
        private final LinearLayout strip;
        private final Context context;
        private final Runnable onPick;
        private String selected;

        public Chips(Context c, List<String> options, String sel, final Runnable onPick) {
            context = c;
            this.onPick = onPick;
            selected = sel;
            strip = Ui.row(c);
            strip.setPadding(0, Ui.dp(c, 2), Ui.dp(c, 6), Ui.dp(c, 2));
            for (String option : options) addOption(option);
            view = new HorizontalScrollView(c);
            view.setHorizontalScrollBarEnabled(false);
            view.setFillViewport(false);
            view.addView(strip);
            paint();
        }

        private void paint() {
            for (int i = 0; i < items.size(); i++) {
                boolean on = opts.get(i).equals(selected);
                TextView item = items.get(i);
                item.setTextColor(on ? Ui.ACCENT : Ui.DIM);
                item.setSelected(on);
                item.setBackground(on
                        ? Ui.strokedRound(item.getContext(), (Ui.ACCENT & 0x00FFFFFF) | 0x33000000, (Ui.ACCENT & 0x00FFFFFF) | 0x59000000, 6)
                        : Ui.strokedRound(item.getContext(), 0x00000000, Ui.CARD_LINE, 6));
            }
        }

        public void addOption(String option) {
            if (opts.contains(option)) return;
            final int index = opts.size();
            opts.add(option);
            TextView t = Ui.tv(context, option, 12.5f, Ui.DIM, false);
            t.setGravity(Gravity.CENTER);
            t.setPadding(Ui.dp(context, 12), Ui.dp(context, 7), Ui.dp(context, 12), Ui.dp(context, 7));
            t.setClickable(true);
            t.setFocusable(true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = Ui.dp(context, 6);
            t.setLayoutParams(lp);
            t.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    pick(index);
                    if (onPick != null) onPick.run();
                }
            });
            items.add(t);
            strip.addView(t);
        }

        public void pick(int i) { selected = opts.get(i); paint(); }

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
                 "按下|按住不放（配合弹起使用）", "弹起|松开按住的手指",
                 "提示|运行时在屏幕上显示一条文本（{变量名} 会替换为当前值）",
                 "框区域|运行时把某个区域框出来（琥珀高亮 + 区域名，自动淡出）",
                 "框矩形|按四个坐标画一个矩形框（坐标可用变量，如找图命中点 fx/fy）"},
                {"判断与视觉", "区域判断|判断某坐标是否在区域内（带两分支）", "开关判断|判断开关型区域是否开着（带两分支）",
                 "跳转|跳到第 N 步 / 结束（旧式循环写法）", "计算|算一个值存进变量",
                 "找图|在屏幕上找图片（命中坐标可给点按用）", "找色|在屏幕上找颜色（单点 / 多点）"},
                {"流程控制", "循环|重复一段步骤（次数 / 时长 / 无限 / 到条件）",
                 "条件块|如果…否则…（支持 且/或 多条件；自动带结束标记）",
                 "跳出循环|立即跳出（可跳多层）", "继续循环|跳过本轮剩余 → 下一轮",
                 "终止|立即结束整个操作（完成收工 / 中止退出）"},
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
