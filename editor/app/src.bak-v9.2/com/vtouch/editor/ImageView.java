package com.vtouch.editor;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;

/**
 * 看图器：缩放(1..8x)/平移/放大镜（仅取色会话启用 setLoupeEnabled）/框选(四角+整体挪)/点选/吸色/点集标注/命中框/十字线。
 * 坐标体系：对外一律「帧像素」（selection/setHit/markers/base）；显示读数经 Matcher.frameToLogic 转竖屏逻辑坐标。
 * 手势：双指捏合缩放（锚点=两指中心）；单指——BOX 模式角手柄缩放/框内挪框/框外平移；点选模式轻点=onPoint、拖动=平移。
 * 口径参照面板（vtouch_ui.cpp:271-279 + EDITOR_V2 §7.2），按触屏习惯升级（捏合/放大镜/高倍最近邻/像素网格）。
 */
public class ImageView extends View {
    public static final int MODE_VIEW = 0, MODE_BOX = 1, MODE_POINT = 2, MODE_PTS = 3;

    public interface Listener { void onBox(int x, int y, int w, int h); void onPoint(int x, int y); }

    private static final float ZOOM_MIN = 1f, ZOOM_MAX = 8f;
    private static final float HANDLE_DP = 24f;
    private static final int BOX_MIN = 8;

    private boolean loupeOn = false;

    /** 放大镜开关（仅取色会话需要）。 */
    public void setLoupeEnabled(boolean on) { loupeOn = on; }

    private Bitmap bmp;
    private int rot;
    private int mode = MODE_VIEW;
    private Listener listener;

    private float zoom = 1f;
    private float panX = 0, panY = 0;
    private final int[] sel = {0, 0, 0, 0};
    private boolean selOn = false;
    private final int[] hit = {-1, 0, 0, 0};
    private final int[] base = {-1, -1};
    private int baseRgb;
    private int[] mx = new int[0], my = new int[0];

    private final ScaleGestureDetector scaleDet;

    private int grab = 0;                       /* 0 无 / 1 挪框 / 2 角缩放 / 3 平移 / 4 拖画新框 */
    private int corner = 0;
    private float nboxX, nboxY;                 /* 新框起点（帧像素浮点） */
    private boolean nboxOn;                     /* 新框已开始拖（防点按误建） */
    private float lastFocusX, lastFocusY;       /* 双指平移基准 */
    private boolean twoFinger;
    private float lastX, lastY, downX, downY;
    private boolean moved;
    private boolean touchActive;
    private float touchFX = -1, touchFY = -1;

    public ImageView(Context c) {
        super(c);
        scaleDet = new ScaleGestureDetector(c, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScale(ScaleGestureDetector d) {
                setZoom(clampF(zoom * d.getScaleFactor(), ZOOM_MIN, ZOOM_MAX), d.getFocusX(), d.getFocusY());
                return true;
            }
        });
    }

    // ---------------------------------------------------------------- 对外

    public void setFrame(Bitmap b, int r) {
        bmp = b;
        rot = r & 3;
        selOn = false;
        hit[0] = -1;
        base[0] = -1;
        mx = new int[0];
        my = new int[0];
        zoom = 1f;
        if (getWidth() > 0) clampPan();
        invalidate();
    }

    public void setMode(int m) { mode = m; invalidate(); }

    public void setListener(Listener l) { listener = l; }

    public void setSelection(int x, int y, int w, int h) {
        if (bmp == null) return;
        int bw = bmpW(), bh = bmpH();
        int ww = clampI(w, BOX_MIN, bw), hh = clampI(h, BOX_MIN, bh);
        int xx = clampI(x, 0, bw - ww), yy = clampI(y, 0, bh - hh);
        sel[0] = xx; sel[1] = yy; sel[2] = ww; sel[3] = hh;
        selOn = true;
        notifyBox();
        invalidate();
    }

    public int[] selection() { return new int[]{sel[0], sel[1], sel[2], sel[3]}; }

    public void setHit(int x, int y, int w, int h) {
        hit[0] = x; hit[1] = y; hit[2] = w; hit[3] = h;
        invalidate();
    }

    public void setBase(int x, int y, int rgb) {
        base[0] = x; base[1] = y; baseRgb = rgb & 0xFFFFFF;
        invalidate();
    }

    public void setMarkers(int[] xs, int[] ys) {
        mx = xs != null ? xs : new int[0];
        my = ys != null ? ys : new int[0];
        invalidate();
    }

    public float zoom() { return zoom; }

    public void zoomStep(int dir) {
        if (getWidth() <= 0) return;
        float z = (dir > 0) ? clampF(zoom * 1.5f, ZOOM_MIN, ZOOM_MAX) : clampF(zoom / 1.5f, ZOOM_MIN, ZOOM_MAX);
        setZoom(z, getWidth() / 2f, getHeight() / 2f);
    }

    /** 以屏幕点 (cxS,cyS) 为锚设置缩放（含钳制与居中收尾）。 */
    public void setZoom(float z, float cxS, float cyS) {
        if (bmp == null) { zoom = clampF(z, ZOOM_MIN, ZOOM_MAX); return; }
        float s = scale();
        float fx = (cxS - panX) / s, fy = (cyS - panY) / s;
        zoom = clampF(z, ZOOM_MIN, ZOOM_MAX);
        float s2 = scale();
        panX = cxS - fx * s2;
        panY = cyS - fy * s2;
        clampPan();
        invalidate();
    }

    public void fit() {
        zoom = 1f;
        clampPan();
        invalidate();
    }

    int[] screenToFrame(int sx, int sy) {
        float s = scale();
        int fx = Math.round((sx - panX) / s);
        int fy = Math.round((sy - panY) / s);
        if (bmp != null) {
            fx = clampI(fx, 0, bmpW() - 1);
            fy = clampI(fy, 0, bmpH() - 1);
        }
        return new int[]{fx, fy};
    }

    int[] frameToScreen(int fx, int fy) {
        float s = scale();
        return new int[]{Math.round(panX + fx * s), Math.round(panY + fy * s)};
    }

    // ---------------------------------------------------------------- 内部

    private int bmpW() { return bmp == null ? 0 : bmp.getWidth(); }
    private int bmpH() { return bmp == null ? 0 : bmp.getHeight(); }

    private float fitScale() {
        if (bmp == null) return 1f;
        int vw = Math.max(1, getWidth()), vh = Math.max(1, getHeight());
        return Math.min((float) vw / bmpW(), (float) vh / bmpH());
    }

    private float scale() { return fitScale() * zoom; }

    private void clampPan() {
        if (bmp == null) { panX = panY = 0; return; }
        float s = scale();
        float cw = bmpW() * s, ch = bmpH() * s;
        int vw = getWidth(), vh = getHeight();
        if (cw <= vw) panX = (vw - cw) / 2f;
        else panX = Math.max(vw - cw, Math.min(0f, panX));
        if (ch <= vh) panY = (vh - ch) / 2f;
        else panY = Math.max(vh - ch, Math.min(0f, panY));
    }

    private void notifyBox() {
        if (listener != null) listener.onBox(sel[0], sel[1], sel[2], sel[3]);
    }

    private int hitCorner(float sx, float sy) {
        int r = Ui.dp(getContext(), HANDLE_DP);
        float s = scale();
        float x1 = panX + sel[0] * s, y1 = panY + sel[1] * s;
        float x2 = panX + (sel[0] + sel[2]) * s, y2 = panY + (sel[1] + sel[3]) * s;
        float[][] cs = {{x1, y1}, {x2, y1}, {x1, y2}, {x2, y2}};
        for (int i = 0; i < 4; i++)
            if (Math.hypot(sx - cs[i][0], sy - cs[i][1]) <= r) return i;
        return -1;
    }

    /** 拖画新框：起点 → 当前点归一化（帧像素，钳在帧内）。 */
    private void updateNewBox(float fx, float fy) {
        if (bmp == null) return;
        int x0 = clampI(Math.round(Math.min(nboxX, fx)), 0, bmpW() - 1);
        int y0 = clampI(Math.round(Math.min(nboxY, fy)), 0, bmpH() - 1);
        int x1 = clampI(Math.round(Math.max(nboxX, fx)), 0, bmpW() - 1);
        int y1 = clampI(Math.round(Math.max(nboxY, fy)), 0, bmpH() - 1);
        sel[0] = x0;
        sel[1] = y0;
        sel[2] = Math.max(1, x1 - x0);
        sel[3] = Math.max(1, y1 - y0);
        selOn = true;
    }

    private boolean insideSel(float fx, float fy) {
        return fx >= sel[0] && fy >= sel[1] && fx <= sel[0] + sel[2] && fy <= sel[1] + sel[3];
    }

    private void resizeByCorner(float dxS, float dyS) {
        float s = scale();
        int dx = Math.round(dxS / s), dy = Math.round(dyS / s);
        int bw = bmpW(), bh = bmpH();
        int x1 = sel[0], y1 = sel[1], x2 = sel[0] + sel[2], y2 = sel[1] + sel[3];
        switch (corner) {
            case 0: x1 += dx; y1 += dy; break;
            case 1: x2 += dx; y1 += dy; break;
            case 2: x1 += dx; y2 += dy; break;
            default: x2 += dx; y2 += dy; break;
        }
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > bw) x2 = bw;
        if (y2 > bh) y2 = bh;
        if (x2 - x1 < BOX_MIN) { if (corner == 0 || corner == 2) x1 = x2 - BOX_MIN; else x2 = x1 + BOX_MIN; }
        if (y2 - y1 < BOX_MIN) { if (corner == 0 || corner == 1) y1 = y2 - BOX_MIN; else y2 = y1 + BOX_MIN; }
        if (x1 < 0) { x1 = 0; x2 = Math.min(bw, BOX_MIN); }
        if (y1 < 0) { y1 = 0; y2 = Math.min(bh, BOX_MIN); }
        if (x2 > bw) { x2 = bw; x1 = Math.max(0, bw - BOX_MIN); }
        if (y2 > bh) { y2 = bh; y1 = Math.max(0, bh - BOX_MIN); }
        sel[0] = x1; sel[1] = y1; sel[2] = x2 - x1; sel[3] = y2 - y1;
    }

    private void updateTouchTarget(float sx, float sy) {
        float s = scale();
        touchFX = (sx - panX) / s;
        touchFY = (sy - panY) / s;
    }

    // ---------------------------------------------------------------- 测量 / 触摸

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int dw = Ui.dp(getContext(), 320), dh = Ui.dp(getContext(), 420);
        setMeasuredDimension(resolveSize(dw, wSpec), resolveSize(dh, hSpec));
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        clampPan();
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (bmp == null) return false;
        scaleDet.onTouchEvent(e);
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = lastX = e.getX();
                downY = lastY = e.getY();
                moved = false;
                touchActive = true;
                if (mode == MODE_BOX) {
                    int c = selOn ? hitCorner(downX, downY) : -1;
                    float s = scale();
                    float fx = (downX - panX) / s, fy = (downY - panY) / s;
                    if (c >= 0) { grab = 2; corner = c; }
                    else if (selOn && insideSel(fx, fy)) { grab = 1; }
                    else {
                        grab = 4;                    /* 框外拖动 = 拖画新框 */
                        nboxX = fx;
                        nboxY = fy;
                        nboxOn = false;
                    }
                } else {
                    grab = 3;
                }
                updateTouchTarget(e.getX(), e.getY());
                invalidate();
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                grab = 0;
                twoFinger = true;
                lastFocusX = (e.getX(0) + e.getX(1)) / 2f;
                lastFocusY = (e.getY(0) + e.getY(1)) / 2f;
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                grab = 0;
                twoFinger = false;
            case MotionEvent.ACTION_MOVE: {
                if (e.getPointerCount() >= 2) {          /* 双指：捏合缩放 + 平移 */
                    float fx2 = (e.getX(0) + e.getX(1)) / 2f;
                    float fy2 = (e.getY(0) + e.getY(1)) / 2f;
                    if (twoFinger) {
                        panX += fx2 - lastFocusX;
                        panY += fy2 - lastFocusY;
                        clampPan();
                    }
                    twoFinger = true;
                    lastFocusX = fx2;
                    lastFocusY = fy2;
                    updateTouchTarget(fx2, fy2);
                    invalidate();
                    return true;
                }
                float x = e.getX(), y = e.getY();
                float dx = x - lastX, dy = y - lastY;
                if (Math.abs(x - downX) > Ui.dp(getContext(), 6) || Math.abs(y - downY) > Ui.dp(getContext(), 6)) moved = true;
                if (grab == 3) {
                    panX += dx;
                    panY += dy;
                    clampPan();
                } else if (grab == 1 && moved) {
                    float s = scale();
                    sel[0] = clampI(sel[0] + Math.round(dx / s), 0, bmpW() - sel[2]);
                    sel[1] = clampI(sel[1] + Math.round(dy / s), 0, bmpH() - sel[3]);
                    notifyBox();
                } else if (grab == 2 && moved) {
                    resizeByCorner(dx, dy);
                    notifyBox();
                } else if (grab == 4 && moved) {
                    float s = scale();
                    nboxOn = true;
                    updateNewBox((x - panX) / s, (y - panY) / s);
                    notifyBox();
                }
                lastX = x;
                lastY = y;
                updateTouchTarget(x, y);
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_UP:
                if (grab == 4 && nboxOn && selOn) {      /* 新框收尾：保底 8×8 */
                    if (sel[2] < BOX_MIN || sel[3] < BOX_MIN) {
                        int cx = sel[0] + sel[2] / 2, cy = sel[1] + sel[3] / 2;
                        int w = Math.max(BOX_MIN, sel[2]), h = Math.max(BOX_MIN, sel[3]);
                        sel[0] = clampI(cx - w / 2, 0, Math.max(0, bmpW() - w));
                        sel[1] = clampI(cy - h / 2, 0, Math.max(0, bmpH() - h));
                        sel[2] = w;
                        sel[3] = h;
                    }
                    notifyBox();
                }
                touchActive = false;
                touchFX = touchFY = -1;
                if (!moved && (mode == MODE_POINT || mode == MODE_PTS) && listener != null) {
                    int[] f = screenToFrame((int) lastX, (int) lastY);
                    listener.onPoint(f[0], f[1]);
                }
                grab = 0;
                invalidate();
                return true;
            case MotionEvent.ACTION_CANCEL:
                touchActive = false;
                touchFX = touchFY = -1;
                grab = 0;
                invalidate();
                return true;
        }
        return super.onTouchEvent(e);
    }

    // ---------------------------------------------------------------- 绘制

    @Override
    protected void onDraw(Canvas c) {
        if (bmp == null) return;
        Context ctx = getContext();
        float s = scale();
        Paint p = new Paint();
        p.setFilterBitmap(zoom < 2f);
        c.drawBitmap(bmp, new Rect(0, 0, bmpW(), bmpH()),
                new RectF(panX, panY, panX + bmpW() * s, panY + bmpH() * s), p);
        // 像素网格（放大 ≥20px/帧像素时）
        if (s >= Ui.dp(ctx, 20)) {
            Paint g = new Paint();
            g.setColor(0x33E9EDF1);
            g.setStrokeWidth(1);
            int x0 = Math.max(0, (int) ((0 - panX) / s)), x1 = Math.min(bmpW() - 1, (int) ((getWidth() - panX) / s));
            int y0 = Math.max(0, (int) ((0 - panY) / s)), y1 = Math.min(bmpH() - 1, (int) ((getHeight() - panY) / s));
            for (int x = x0; x <= x1; x++) {
                float sx = panX + x * s;
                if (sx >= 0 && sx <= getWidth()) c.drawLine(sx, 0, sx, getHeight(), g);
            }
            for (int y = y0; y <= y1; y++) {
                float sy = panY + y * s;
                if (sy >= 0 && sy <= getHeight()) c.drawLine(0, sy, getWidth(), sy, g);
            }
        }
        // 选区（遮罩 + 框 + 四角手柄）
        if (selOn && mode == MODE_BOX) {
            float x1 = panX + sel[0] * s, y1 = panY + sel[1] * s;
            float x2 = panX + (sel[0] + sel[2]) * s, y2 = panY + (sel[1] + sel[3]) * s;
            Paint scrim = new Paint();
            scrim.setColor(0x66000000);
            c.drawRect(0, 0, getWidth(), y1, scrim);
            c.drawRect(0, y2, getWidth(), getHeight(), scrim);
            c.drawRect(0, y1, x1, y2, scrim);
            c.drawRect(x2, y1, getWidth(), y2, scrim);
            Paint box = new Paint(Paint.ANTI_ALIAS_FLAG);
            box.setStyle(Paint.Style.STROKE);
            box.setStrokeWidth(Ui.dp(ctx, 1.5f));
            box.setColor(Ui.ACCENT);
            c.drawRect(new RectF(x1, y1, x2, y2), box);
            float hs = Ui.dp(ctx, 10);
            Paint hf = new Paint(Paint.ANTI_ALIAS_FLAG);
            for (float[] pt : new float[][]{{x1, y1}, {x2, y1}, {x1, y2}, {x2, y2}}) {
                hf.setColor(0xFFFFFFFF);
                c.drawRect(pt[0] - hs / 2, pt[1] - hs / 2, pt[0] + hs / 2, pt[1] + hs / 2, hf);
                hf.setStyle(Paint.Style.STROKE);
                hf.setStrokeWidth(Ui.dp(ctx, 2));
                hf.setColor(Ui.ACCENT);
                c.drawRect(pt[0] - hs / 2, pt[1] - hs / 2, pt[0] + hs / 2, pt[1] + hs / 2, hf);
                hf.setStyle(Paint.Style.FILL);
            }
        }
        // 命中框（绿）
        if (hit[0] >= 0) {
            Paint hp = new Paint(Paint.ANTI_ALIAS_FLAG);
            hp.setStyle(Paint.Style.STROKE);
            hp.setStrokeWidth(Ui.dp(ctx, 2));
            hp.setColor(Ui.OK);
            c.drawRect(new RectF(panX + hit[0] * s, panY + hit[1] * s,
                    panX + (hit[0] + hit[2]) * s, panY + (hit[1] + hit[3]) * s), hp);
        }
        // 点集：基准 + 参考点
        if (base[0] >= 0) {
            Paint bp = new Paint(Paint.ANTI_ALIAS_FLAG);
            bp.setStyle(Paint.Style.STROKE);
            bp.setStrokeWidth(Ui.dp(ctx, 2));
            bp.setColor(0xFF000000 | baseRgb);
            float bx = panX + base[0] * s, by = panY + base[1] * s;
            c.drawCircle(bx, by, Ui.dp(ctx, 9), bp);
            c.drawLine(bx - Ui.dp(ctx, 14), by, bx + Ui.dp(ctx, 14), by, bp);
            c.drawLine(bx, by - Ui.dp(ctx, 14), bx, by + Ui.dp(ctx, 14), bp);
        }
        for (int i = 0; i < mx.length && i < my.length; i++) {
            Paint mp = new Paint(Paint.ANTI_ALIAS_FLAG);
            mp.setStrokeWidth(Ui.dp(ctx, 2));
            mp.setColor(Ui.TEXT);
            float px = panX + mx[i] * s, py = panY + my[i] * s;
            c.drawLine(px - Ui.dp(ctx, 7), py, px + Ui.dp(ctx, 7), py, mp);
            c.drawLine(px, py - Ui.dp(ctx, 7), px, py + Ui.dp(ctx, 7), mp);
        }
        // 放大镜（仅取色会话；其余模式不打扰）
        if (loupeOn && touchActive && touchFX >= 0) drawLoupe(c);
    }

    private void drawLoupe(Canvas c) {
        Context ctx = getContext();
        int R = Ui.dp(ctx, 60);
        float cx = lastX, cy = lastY - R * 1.7f;
        if (cy - R < Ui.dp(ctx, 8)) cy = lastY + R * 1.7f;
        cx = clampF(cx, R + Ui.dp(ctx, 4), getWidth() - R - Ui.dp(ctx, 4));
        float s = scale();
        float s2 = s * 4f;
        c.save();
        Path clip = new Path();
        clip.addCircle(cx, cy, R, Path.Direction.CW);
        c.clipPath(clip);
        Paint bg = new Paint();
        bg.setColor(0xE00C0E10);
        c.drawCircle(cx, cy, R, bg);
        Paint p = new Paint();
        p.setFilterBitmap(false);
        float dx = cx - touchFX * s2, dy = cy - touchFY * s2;
        c.drawBitmap(bmp, new Rect(0, 0, bmpW(), bmpH()),
                new RectF(dx, dy, dx + bmpW() * s2, dy + bmpH() * s2), p);
        c.restore();
        Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(Ui.dp(ctx, 2));
        ring.setColor(Ui.ACCENT);
        c.drawCircle(cx, cy, R, ring);
        c.drawLine(cx - Ui.dp(ctx, 12), cy, cx + Ui.dp(ctx, 12), cy, ring);
        c.drawLine(cx, cy - Ui.dp(ctx, 12), cx, cy + Ui.dp(ctx, 12), ring);
        int[] lg = new int[2];
        Matcher.frameToLogic(rot, bmpW(), bmpH(), Math.round(touchFX), Math.round(touchFY), lg);
        Paint tp = new Paint(Paint.ANTI_ALIAS_FLAG);
        tp.setColor(Ui.TEXT);
        tp.setTextSize(Ui.dp(ctx, 11));
        String txt = "帧 " + Math.round(touchFX) + "," + Math.round(touchFY) + " · 逻辑 " + lg[0] + "," + lg[1];
        float tw = tp.measureText(txt);
        float tx = cx + R + Ui.dp(ctx, 6);
        if (tx + tw > getWidth() - Ui.dp(ctx, 4)) tx = cx - R - Ui.dp(ctx, 6) - tw;
        c.drawText(txt, tx, cy, tp);
    }

    // ---------------------------------------------------------------- 小工具

    private static float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    private static int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}
