package com.vtouch.editor;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.List;

/**
 * 公共 UI 工具（v5 控制台风，遵循 impeccable · android.md / operate.md）。
 * 口径：M3 触控目标 ≥48dp（密集行内 40dp）、间距 8dp 网格、涟漪反馈、
 * 字号阶梯 20/16/14/13/12/11、强调色只用于主操作与选中态、标签胶囊描边式。
 *
 * 注意：按钮一律用 TextView 实现（而非 Button 控件）——Material 的 Button 样式
 * 带 88dp 强制最小宽度，在部分 ROM 上 setMinimumWidth 无法覆盖，会破坏紧凑布局。
 */
public final class Ui {
    public static final int BG = 0xFF0C0E10, HEADER = 0xFF14171B, CARD = 0xFF14171A, CARD2 = 0xFF1D2126,
            TEXT = 0xFFE9EDF1, DIM = 0xFF9BA7B4, FAINT = 0xFF6E7986, ACCENT = 0xFFE8B14F,
            ACCENT_PRESS = 0xFFC9973F, ON_ACCENT = 0xFF17130A,
            OK = 0xFF3ECF8E, WARN = 0xFFF07178, LINE = 0xFF22262B, CARD_LINE = 0xFF272D34;

    public static final Handler MAIN = new Handler(Looper.getMainLooper());

    public static int dp(Context c, float v) {
        return (int) (v * c.getResources().getDisplayMetrics().density + 0.5f);
    }

    /** 圆角纯色背景。 */
    public static GradientDrawable round(Context c, int color, float radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(c, radiusDp));
        return g;
    }

    /** 涟漪反馈的圆角背景（M3 交互反馈；不透明按压色自动转为 35% 透明度的涟漪层）。 */
    public static Drawable pressable(Context c, int normal, int pressed, float radiusDp) {
        int ripple = (pressed >>> 24) == 0xFF ? (pressed & 0x00FFFFFF) | 0x59000000 : pressed;
        return new RippleDrawable(ColorStateList.valueOf(ripple),
                round(c, normal, radiusDp), round(c, 0xFFFFFFFF, radiusDp));
    }

    /** 圆角 + 1dp 描边（卡片层次的统一口径）。 */
    public static GradientDrawable strokedRound(Context c, int fill, int strokeColor, float radiusDp) {
        GradientDrawable g = round(c, fill, radiusDp);
        g.setStroke(dp(c, 1), strokeColor);
        return g;
    }

    /** 描边卡片的可点版本（涟漪 + 描边并存）。 */
    public static Drawable pressableStroked(Context c, int fill, int strokeColor, int pressed, float radiusDp) {
        int ripple = (pressed >>> 24) == 0xFF ? (pressed & 0x00FFFFFF) | 0x59000000 : pressed;
        return new RippleDrawable(ColorStateList.valueOf(ripple),
                strokedRound(c, fill, strokeColor, radiusDp), round(c, 0xFFFFFFFF, radiusDp));
    }

    /** 竖向渐变背景（头部区块用）。 */
    public static GradientDrawable vGradient(int top, int bottom) {
        return new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{top, bottom});
    }

    /** 页签选中下划线（2dp 琥珀短线，贴底居中；页签胶囊式 → 下划线式的口径）。 */
    public static Drawable underlineDot(Context c) {
        GradientDrawable line = round(c, ACCENT, 1);
        android.graphics.drawable.LayerDrawable ld =
                new android.graphics.drawable.LayerDrawable(new Drawable[]{line});
        ld.setLayerGravity(0, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        ld.setLayerWidth(0, dp(c, 26));
        ld.setLayerHeight(0, dp(c, 2));
        return ld;
    }

    private static Typeface BRAND_F, MONO_F;

    /** 品牌字标字体（Geist Medium；资源缺失时回退系统）。 */
    public static Typeface brandFont(Context c) {
        if (BRAND_F == null) BRAND_F = loadFont(c, "fonts/geist-medium.ttf", Typeface.SANS_SERIF);
        return BRAND_F;
    }

    /** 数据等宽字体（JetBrains Mono；资源缺失时回退系统等宽）。 */
    public static Typeface monoFont(Context c) {
        if (MONO_F == null) MONO_F = loadFont(c, "fonts/jetbrains-mono.ttf", Typeface.MONOSPACE);
        return MONO_F;
    }

    private static Typeface loadFont(Context c, String asset, Typeface fallback) {
        try {
            return Typeface.createFromAsset(c.getAssets(), asset);
        } catch (Throwable t) {
            return fallback;
        }
    }

    /** 品牌记号：区域方点（圆角方框 + 中心触点）。 */
    public static Drawable brandMark(Context c) {
        return new MarkDrawable(c);
    }

    public static Drawable brandMark(Context c, int color) {
        return new MarkDrawable(c, color);
    }

    /** 记号绘制：按 bounds 方形自适应，软件渲染可画。 */
    static final class MarkDrawable extends Drawable {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float stroke, corner, dotR;

        MarkDrawable(Context c) {
            this(c, ACCENT);
        }

        MarkDrawable(Context c, int color) {
            stroke = dp(c, 1.5f);
            corner = dp(c, 5f);
            dotR = dp(c, 2.4f);
            p.setColor(color);
            p.setStrokeWidth(stroke);
            p.setStyle(Paint.Style.STROKE);
        }

        @Override
        public void draw(Canvas canvas) {
            Rect b = getBounds();
            float s = Math.min(b.width(), b.height());
            float l = b.left + (b.width() - s) / 2f + stroke / 2f;
            float t = b.top + (b.height() - s) / 2f + stroke / 2f;
            float r = l + s - stroke;
            float bt = t + s - stroke;
            canvas.drawRoundRect(l, t, r, bt, corner, corner, p);
            p.setStyle(Paint.Style.FILL);
            canvas.drawCircle((l + r) / 2f, (t + bt) / 2f, dotR, p);
            p.setStyle(Paint.Style.STROKE);
        }

        @Override public void setAlpha(int a) { p.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter cf) { p.setColorFilter(cf); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** 状态点 + 光晕（运行态；RadialGradient 手画，软渲染可画）。 */
    public static Drawable glowDot(Context c, int fg) {
        GradientDrawable halo = new GradientDrawable();
        halo.setShape(GradientDrawable.OVAL);
        halo.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        halo.setGradientRadius(dp(c, 12));
        halo.setColors(new int[]{(fg & 0x00FFFFFF) | 0x66000000, fg & 0x00FFFFFF});
        GradientDrawable dot = round(c, fg, 999);
        android.graphics.drawable.LayerDrawable ld =
                new android.graphics.drawable.LayerDrawable(new Drawable[]{halo, dot});
        int e = dp(c, 4);
        ld.setLayerInset(0, -e, -e, -e, -e);
        return ld;
    }

    /** 胶囊 + 外发光（RadialGradient 光晕层 + 描边底；软渲染可画）。 */
    public static Drawable glowChip(Context c, int fg) {
        GradientDrawable halo = new GradientDrawable();
        halo.setShape(GradientDrawable.OVAL);
        halo.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        halo.setGradientRadius(dp(c, 48));
        halo.setColors(new int[]{(fg & 0x00FFFFFF) | 0x59000000, fg & 0x00FFFFFF});
        Drawable body = strokedRound(c, 0x00000000, (fg & 0x00FFFFFF) | 0x59000000, 4);
        android.graphics.drawable.LayerDrawable ld =
                new android.graphics.drawable.LayerDrawable(new Drawable[]{halo, body});
        int e = dp(c, 6);
        ld.setLayerInset(0, -e, -e, -e, -e);
        return ld;
    }

    /** 当前方案卡：琥珀描边 + 顶部 1dp 内高光。 */
    public static Drawable cardGlow(Context c) {
        GradientDrawable body = strokedRound(c, CARD, (ACCENT & 0x00FFFFFF) | 0x80000000, 8);
        GradientDrawable top = round(c, (ACCENT & 0x00FFFFFF) | 0x33000000, 1);
        android.graphics.drawable.LayerDrawable ld =
                new android.graphics.drawable.LayerDrawable(new Drawable[]{body, top});
        ld.setLayerGravity(1, Gravity.TOP);
        ld.setLayerHeight(1, dp(c, 1));
        ld.setLayerInset(1, dp(c, 10), dp(c, 1), dp(c, 10), 0);
        return ld;
    }

    /** 动态更新胶囊着色（文字 + 1px 描边式；圆角与 chip() 保持一致）。 */
    public static void chipTint(TextView t, int fg) {
        t.setTextColor(fg);
        t.setBackground(strokedRound(t.getContext(), 0x00000000, (fg & 0x00FFFFFF) | 0x59000000, 4));
    }

    public static TextView tv(Context c, CharSequence s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(t.getTypeface(), Typeface.BOLD);
        return t;
    }

    /** 等宽字体文本（坐标、日志等测量/数据内容）。 */
    public static TextView mono(Context c, CharSequence s, float sp, int color) {
        TextView t = tv(c, s, sp, color, false);
        t.setTypeface(monoFont(c));
        return t;
    }

    public static TextView label(Context c, String s) {
        TextView t = tv(c, s, 12, DIM, false);
        t.setPadding(0, dp(c, 8), 0, dp(c, 2));
        return t;
    }

    public static LinearLayout col(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    public static LinearLayout row(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    /** 卡片：圆角 8、1dp 描边、内边距 12、列表间距 8/4。 */
    public static LinearLayout card(Context c) {
        LinearLayout l = col(c);
        l.setBackground(strokedRound(c, CARD, CARD_LINE, 8));
        int p = dp(c, 12);
        l.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(c, 8), dp(c, 4), dp(c, 8), dp(c, 4));
        l.setLayoutParams(lp);
        return l;
    }

    /** 面板：次级容器（日志台），比卡片更平，带描边。 */
    public static LinearLayout panel(Context c) {
        LinearLayout l = col(c);
        l.setBackground(strokedRound(c, CARD2, CARD_LINE, 8));
        int p = dp(c, 12);
        l.setPadding(p, p, p, p);
        return l;
    }

    /** 按钮基座（TextView 实现：无系统样式干扰，宽度完全由内容+最小宽决定）。 */
    private static TextView baseBtn(Context c, String s) {
        TextView b = new TextView(c);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(13);
        b.setTextColor(TEXT);
        b.setGravity(Gravity.CENTER);
        b.setBackground(pressableStroked(c, 0x00000000, CARD_LINE, 0xFF2A3138, 6));
        b.setPadding(dp(c, 14), dp(c, 4), dp(c, 14), dp(c, 4));
        b.setMinimumWidth(dp(c, 48));
        b.setMinimumHeight(dp(c, 48));
        b.setClickable(true);
        b.setFocusable(true);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = dp(c, 8);
        b.setLayoutParams(lp);
        return b;
    }

    /** 次级按钮（灰色实心，默认）。 */
    public static TextView btn(Context c, String s) {
        return baseBtn(c, s);
    }

    /** 主操作按钮（强调色实心）。 */
    public static TextView btnPrimary(Context c, String s) {
        TextView b = baseBtn(c, s);
        b.setTextColor(ON_ACCENT);
        b.setBackground(pressable(c, ACCENT, ACCENT_PRESS, 6));
        return b;
    }

    /** 小号次级按钮（工具行；密集场景 40dp 高，宽度 ≥48dp）。 */
    public static TextView btnSmall(Context c, String s) {
        TextView b = baseBtn(c, s);
        b.setTextSize(12.5f);
        b.setPadding(dp(c, 12), dp(c, 2), dp(c, 12), dp(c, 2));
        b.setMinimumHeight(dp(c, 40));
        return b;
    }

    /** 小号主操作按钮（强调色实心，工具行内）。 */
    public static TextView btnSmallPrimary(Context c, String s) {
        TextView b = btnSmall(c, s);
        b.setTextColor(ON_ACCENT);
        b.setBackground(pressable(c, ACCENT, ACCENT_PRESS, 6));
        return b;
    }

    /** 小号柔色按钮（危险/次操作：20% 色底 + 彩色文字，对话框用）。 */
    public static TextView btnSmallTonal(Context c, String s, int fg) {
        TextView b = btnSmall(c, s);
        b.setTextColor(fg);
        b.setBackground(pressable(c, (fg & 0x00FFFFFF) | 0x2E000000,
                (fg & 0x00FFFFFF) | 0x55000000, 6));
        return b;
    }

    /** 文字按钮（透明底 + 涟漪；用于行内三级操作：编辑/删除/重命名/上下移…）。 */
    public static TextView btnFlat(Context c, String s, int color) {
        TextView b = new TextView(c);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(13);
        b.setTextColor(color);
        b.setGravity(Gravity.CENTER);
        b.setBackground(pressable(c, 0x00000000, 0x40FFFFFF, 6));
        b.setPadding(dp(c, 10), dp(c, 4), dp(c, 10), dp(c, 4));
        b.setMinimumWidth(dp(c, 48));
        b.setMinimumHeight(dp(c, 40));
        b.setClickable(true);
        b.setFocusable(true);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = dp(c, 4);
        b.setLayoutParams(lp);
        return b;
    }

    /** 标签胶囊（描边式：1px 描边 + 着色文字，透明底）。 */
    public static TextView chip(Context c, String s, int fg) {
        TextView t = tv(c, s, 11f, fg, false);
        t.setIncludeFontPadding(false);
        t.setPadding(dp(c, 8), dp(c, 4), dp(c, 8), dp(c, 4));
        t.setBackground(strokedRound(c, 0x00000000, (fg & 0x00FFFFFF) | 0x59000000, 4));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = dp(c, 6);
        t.setLayoutParams(lp);
        return t;
    }

    /** 实心胶囊（唯一高亮位：如「当前」；实底 + 深字）。 */
    public static TextView chipSolid(Context c, String s, int bg, int fg) {
        TextView t = tv(c, s, 11f, fg, true);
        t.setIncludeFontPadding(false);
        t.setPadding(dp(c, 8), dp(c, 4), dp(c, 8), dp(c, 4));
        t.setBackground(round(c, bg, 4));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = dp(c, 6);
        t.setLayoutParams(lp);
        return t;
    }

    /** 等宽加粗（ID / 编号类标题）。 */
    public static TextView monoBold(Context c, CharSequence s, float sp, int color) {
        TextView t = mono(c, s, sp, color);
        t.setTypeface(monoFont(c), Typeface.BOLD);
        return t;
    }

    /** 空状态：标题 + 说明 + 行动按钮（教会用户下一步）。 */
    public static LinearLayout emptyState(Context c, String title, String desc, String btnText,
                                          View.OnClickListener onClick) {
        LinearLayout l = col(c);
        l.setGravity(Gravity.CENTER_HORIZONTAL);
        l.setPadding(dp(c, 32), dp(c, 56), dp(c, 32), dp(c, 32));
        TextView t = tv(c, title, 15, TEXT, true);
        t.setGravity(Gravity.CENTER);
        l.addView(t, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView d = tv(c, desc, 13, DIM, false);
        d.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        dlp.topMargin = dp(c, 8);
        l.addView(d, dlp);
        TextView b = btn(c, btnText);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(c, 20);
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        b.setLayoutParams(lp);
        b.setOnClickListener(onClick);
        l.addView(b);
        return l;
    }

    // ---------------- 统一风格对话框（窗体外壳 + 标题/内容/按钮行全部自绘） ----------------

    /** 对话框壳：标题 + 内容区（可滚） + 按钮行；window 也换成 App 卡片面。 */
    public static final class Dlg {
        public final android.app.AlertDialog dialog;
        public final LinearLayout root;
        public final LinearLayout body;
        public final LinearLayout actions;

        Dlg(android.app.AlertDialog d, LinearLayout r, LinearLayout b, LinearLayout a) {
            dialog = d;
            root = r;
            body = b;
            actions = a;
        }

        /** 显示：宽度 90% + 去掉系统对话框底（window/面板两层），露出自绘卡面。 */
        public void show() {
            android.util.Log.i("vtouch-dlg", "Dlg.show() enter");
            android.view.Window w = dialog.getWindow();
            if (w != null) {
                w.setDimAmount(0.5f);
                int wd = (int) (w.getDecorView().getResources().getDisplayMetrics().widthPixels * 0.90);
                w.setLayout(wd, android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
            }
            dialog.show();
            android.util.Log.i("vtouch-dlg", "Dlg.show() after dialog.show()");
            if (w != null) {
                w.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(0x00000000));
                android.view.View decor = w.getDecorView();
                if (decor instanceof android.view.ViewGroup) {
                    clearBg((android.view.ViewGroup) decor, 3);
                }
                int wd2 = (int) (w.getDecorView().getResources().getDisplayMetrics().widthPixels * 0.90);
                w.setLayout(wd2, android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
            }
            android.util.Log.i("vtouch-dlg", "Dlg.show() done");
        }

        public void dismiss() {
            android.util.Log.i("vtouch-dlg", "Dlg.dismiss()");
            dialog.dismiss();
        }
    }

    /** 递归清除系统对话框面板背景（清到第二层即止；自绘卡面在更深层，不受影响）。 */
    private static void clearBg(android.view.ViewGroup g, int depth) {
        for (int i = 0; i < g.getChildCount(); i++) {
            android.view.View ch = g.getChildAt(i);
            ch.setBackground(null);
            if (depth > 1 && ch instanceof android.view.ViewGroup) {
                clearBg((android.view.ViewGroup) ch, depth - 1);
            }
        }
    }

    /** 构建统一风格的对话框（不调用 show；调用方填 body/actions 后再 show）。 */
    public static Dlg dialog(Context c, String title) {
        LinearLayout root = col(c);
        int px = dp(c, 22);
        root.setPadding(px, dp(c, 20), px, dp(c, 10));
        root.setBackground(strokedRound(c, CARD, CARD_LINE, 12));
        root.addView(tv(c, title, 18, TEXT, true));
        LinearLayout body = col(c);
        android.widget.ScrollView sv = new android.widget.ScrollView(c);
        sv.addView(body);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(c, 10);
        root.addView(sv, blp);
        LinearLayout actions = row(c);
        actions.setGravity(Gravity.END);
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        alp.topMargin = dp(c, 14);
        root.addView(actions, alp);
        android.app.AlertDialog d = new android.app.AlertDialog.Builder(c).setView(root).create();
        return new Dlg(d, root, body, actions);
    }

    /** 对话框说明文字。 */
    public static TextView dlgMessage(Context c, String s) {
        TextView t = tv(c, s, 13, DIM, false);
        t.setLineSpacing(dp(c, 4), 1f);
        return t;
    }

    /** 数字输入（允许负号：变量编码）。 */
    public static EditText numField(Context c, String hint) {
        EditText e = new EditText(c);
        e.setHint(hint);
        e.setHintTextColor(DIM);
        e.setTextColor(TEXT);
        e.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED);
        e.setBackground(strokedRound(c, CARD2, CARD_LINE, 6));
        e.setPadding(dp(c, 10), dp(c, 8), dp(c, 10), dp(c, 8));
        return e;
    }

    /** 文本输入（单行）。 */
    public static EditText textField(Context c, String hint) {
        EditText e = new EditText(c);
        e.setHint(hint);
        e.setHintTextColor(DIM);
        e.setTextColor(TEXT);
        e.setInputType(InputType.TYPE_CLASS_TEXT);
        e.setSingleLine(true);
        e.setBackground(strokedRound(c, CARD2, CARD_LINE, 6));
        e.setPadding(dp(c, 10), dp(c, 8), dp(c, 10), dp(c, 8));
        return e;
    }

    public static View divider(Context c) {
        View v = new View(c);
        v.setBackgroundColor(LINE);
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(c, 1)));
        return v;
    }

    /** 后台跑 work，完成后（可选）回主线程跑 done。 */
    public static void bg(final Runnable work, final Runnable done) {
        new Thread(new Runnable() {
            public void run() {
                try {
                    work.run();
                } finally {
                    if (done != null) MAIN.post(done);
                }
            }
        }).start();
    }

    /** 前台宿主（由 MainActivity 设置；无头渲染服务中为 null，回调静默）。 */
    public interface Host {
        void toast(String s);
        void saved(String what);
        void refreshHeader();
        void runSelfTestFlow();
    }

    public static MainActivity HOST;

    public static void toast(String s) { if (HOST != null) HOST.toast(s); }

    public static void saved(String what) { if (HOST != null) HOST.saved(what); }

    public static void refreshHeader() { if (HOST != null) HOST.refreshHeader(); }

    public static void runSelfTestFlow() { if (HOST != null) HOST.runSelfTestFlow(); }

    /** Spinner 深色适配：收起态文字用亮色。 */
    public static class DarkAdapter extends ArrayAdapter<String> {
        public DarkAdapter(Context c, List<String> items) {
            super(c, android.R.layout.simple_spinner_item, items);
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            View v = super.getView(position, convertView, parent);
            if (v instanceof TextView) ((TextView) v).setTextColor(TEXT);
            return v;
        }
    }
}
