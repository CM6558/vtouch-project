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

import java.util.ArrayList;
import java.util.List;

/**
 * 看图器：缩放(1..8x)/平移/放大镜（仅取色会话启用 setLoupeEnabled）/框选(四角+整体挪)/点选/吸色/点集标注/命中框/十字线。
 * 坐标体系：对外一律「帧像素」（selection/setHit/markers/base）；显示读数经 Matcher.frameToLogic 转竖屏逻辑坐标。
 * 手势：双指捏合缩放（锚点=两指中心）；单指——BOX 模式角手柄缩放/框内挪框/框外平移；点选模式轻点=onPoint、拖动=平移。
 * 口径参照面板（vtouch_ui.cpp:271-279 + EDITOR_V2 §7.2），按触屏习惯升级（捏合/放大镜/高倍最近邻/像素网格）。
 */
public class ImageView extends View {
    public static final int MODE_VIEW = 0, MODE_BOX = 1, MODE_POINT = 2, MODE_PTS = 3, MODE_CIRCLE = 4,
            MODE_REGIONS = 5;   /* v12c：全部区域总览调整 */

    public interface Listener { void onBox(int x, int y, int w, int h); void onPoint(int x, int y); }

    /** v12 [框区域] 圆形：圆心/半径变化回调（帧像素）。 */
    public interface CircleListener { void onCircle(int cx, int cy, int r); }

    /** v12c 总览区域件（帧像素；rect: g={x,y,w,h}；circle: g={cx,cy,r,0}）。v13：带属性编辑字段。 */
    public static class RegShape {
        public String id;
        public String origId;                       /* 盘上原名（新画件 null；改名后 id≠origId） */
        public boolean circle;
        public boolean en = true;
        public boolean moved;                       /* 本会话内几何被调整过（保存仅回写这些，未动的保持盘上原值） */
        public String bindOp = "-", bindEv = "down";/* v13：属性（随保存写盘） */
        public int kind = 0;
        public boolean propsDirty;                  /* v13：属性/改名待写 */
        public final int[] g = new int[4];
    }

    /** v12c 总览回调：选中变化 / 几何变化（拖动缩放）。 */
    public interface RegionsListener {
        void onRegionSelected(int idx);
        void onRegionGeometryChanged();
    }

    private static final float ZOOM_MIN = 1f, ZOOM_MAX = 8f;
    private static final float OVERPAN = 0.35f;   /* v13.1：放大时允许过拖（视口比例）——角落能拖离缩放胶囊下方 */
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
    /* v12 圆形（MODE_CIRCLE）：帧像素圆心/半径 */
    private final int[] csel = {0, 0, 0};
    private boolean cselOn = false;
    private CircleListener circleListener;
    /* v12c 总览（MODE_REGIONS）：全部区域同屏显示/分别调整 */
    private final ArrayList<RegShape> rShapes = new ArrayList<RegShape>();
    private int rSel = -1;
    private RegionsListener regionsListener;
    private int rNewArm = 0;                    /* v12d：待画新形状（0 无 / 1 矩形 / 2 圆） */
    private final int[] rNewTmp = {0, 0, 0, 0}; /* 拖画中的临时几何（rect x,y,w,h / circle cx,cy,r,_） */
    private boolean rNewOn = false;
    private float rNewX0, rNewY0;
    private final RegShape rNewGhost = new RegShape();
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

    /* v13：无底图空间（网格画布）+ 底图隐藏 + 屏外边缘标记（{sx,sy,idx}） */
    private int spaceW = 0, spaceH = 0;
    private boolean bgHidden = false;
    private final ArrayList<float[]> offMarks = new ArrayList<float[]>();

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
        cselOn = false;
        csel[2] = 0;
        rShapes.clear();
        rSel = -1;
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
    public void setCircleListener(CircleListener l) { circleListener = l; }

    public void setSelection(int x, int y, int w, int h) {
        if (bmp == null && spaceW <= 0) return;
        int bw = bmpW(), bh = bmpH();
        int ww = clampI(w, BOX_MIN, bw), hh = clampI(h, BOX_MIN, bh);
        int xx = clampI(x, 0, bw - ww), yy = clampI(y, 0, bh - hh);
        sel[0] = xx; sel[1] = yy; sel[2] = ww; sel[3] = hh;
        selOn = true;
        notifyBox();
        invalidate();
    }

    public int[] selection() { return new int[]{sel[0], sel[1], sel[2], sel[3]}; }

    /** 清空选区（[重框] 用；不影响缩放/平移）。 */
    public void clearSelection() {
        selOn = false;
        invalidate();
    }

    /** v12 圆形：置圆心/半径（帧像素，钳在帧内）。 */
    public void setCircle(int cx, int cy, int r) {
        if (bmp == null && spaceW <= 0) return;
        csel[0] = clampI(cx, 0, bmpW());
        csel[1] = clampI(cy, 0, bmpH());
        csel[2] = clampI(r, 0, Math.max(bmpW(), bmpH()));
        cselOn = true;
        notifyCircle();
        invalidate();
    }

    public int[] circle() { return new int[]{csel[0], csel[1], csel[2]}; }

    /** 清空圆（[重框] 用）。 */
    public void clearCircle() {
        cselOn = false;
        csel[2] = 0;
        invalidate();
    }

    /* ---- v12c 总览 API ---- */

    /** 置全部区域件（克隆入内部表；清选中）。 */
    public void setRegions(List<RegShape> shapes) {
        rShapes.clear();
        if (shapes != null) rShapes.addAll(shapes);
        rSel = -1;
        invalidate();
    }

    /** 内部件表（拖动即改；调用方保存时直接读）。 */
    public List<RegShape> regions() { return rShapes; }

    public int selectedRegion() { return rSel; }

    public void setRegionsListener(RegionsListener l) { regionsListener = l; }

    /** 测试/渲染用：置选中（不触发回调）。 */
    void selectRegionForTest(int idx) {
        rSel = (idx >= -1 && idx < rShapes.size()) ? idx : -1;
        invalidate();
    }

    /** v13：置选中（正式名；不触发回调）。 */
    public void selectRegion(int idx) { selectRegionForTest(idx); }

    /** v13：按对象删除一个件（删除落点不怕索引漂移）。 */
    public void removeShape(RegShape sh) {
        if (sh == null) return;
        rShapes.remove(sh);
        if (rSel >= rShapes.size()) rSel = -1;
        invalidate();
    }

    /** v13：完全落在画面区外的件数（与边缘标记同判据）。 */
    public int regionOffCount() {
        int W = bmpW(), H = bmpH(), n = 0;
        for (RegShape sh : rShapes) {
            boolean out = sh.circle
                    ? (sh.g[0] + sh.g[2] <= 0 || sh.g[0] - sh.g[2] >= W || sh.g[1] + sh.g[2] <= 0 || sh.g[1] - sh.g[2] >= H)
                    : (sh.g[0] + sh.g[2] <= 0 || sh.g[0] >= W || sh.g[1] + sh.g[3] <= 0 || sh.g[1] >= H);
            if (out) n++;
        }
        return n;
    }

    /** v13：置无底图空间（画布坐标系 = w×h，网格显示；bmp 置空，调用方随后重设件表）。 */
    public void setSpace(int w, int h) {
        bmp = null;
        spaceW = Math.max(1, w); spaceH = Math.max(1, h);
        selOn = false;
        cselOn = false;
        csel[2] = 0;
        rShapes.clear();
        rSel = -1;
        hit[0] = -1;
        base[0] = -1;
        mx = new int[0];
        my = new int[0];
        zoom = 1f;
        if (getWidth() > 0) clampPan();
        invalidate();
    }

    public void setBgHidden(boolean hide) { bgHidden = hide; invalidate(); }
    public boolean bgHidden() { return bgHidden; }
    public boolean inSpaceMode() { return bmp == null && spaceW > 0; }

    /** v12d：置新形状画具（0 关 / 1 矩形 / 2 圆）。 */
    public void setNewArm(int v) {
        rNewArm = v == 1 ? 1 : (v == 2 ? 2 : 0);
        invalidate();
    }

    public int newArm() { return rNewArm; }

    /* ---- v12d 新形状拖画三件（DOWN/MOVE/UP 调用；包内可见，测试可直吃帧坐标） ---- */

    void rNewBegin(float fx, float fy) {
        rNewOn = true;
        rNewX0 = fx;
        rNewY0 = fy;
        if (rNewArm == 2) {
            rNewTmp[0] = clampI(Math.round(fx), 0, bmpW());
            rNewTmp[1] = clampI(Math.round(fy), 0, bmpH());
            rNewTmp[2] = 0;
            rNewTmp[3] = 0;
        } else {
            rNewTmp[0] = clampI(Math.round(fx), 0, Math.max(0, bmpW() - 1));
            rNewTmp[1] = clampI(Math.round(fy), 0, Math.max(0, bmpH() - 1));
            rNewTmp[2] = 1;
            rNewTmp[3] = 1;
        }
    }

    void rNewMove(float fx, float fy) {
        if (!rNewOn) return;
        if (rNewArm == 2) {
            rNewTmp[2] = clampI((int) Math.round(Math.hypot(fx - rNewTmp[0], fy - rNewTmp[1])),
                    0, Math.max(bmpW(), bmpH()));
        } else {
            int x0 = clampI(Math.round(Math.min(rNewX0, fx)), 0, Math.max(0, bmpW() - 1));
            int y0 = clampI(Math.round(Math.min(rNewY0, fy)), 0, Math.max(0, bmpH() - 1));
            int x1 = clampI(Math.round(Math.max(rNewX0, fx)), 0, Math.max(0, bmpW() - 1));
            int y1 = clampI(Math.round(Math.max(rNewY0, fy)), 0, Math.max(0, bmpH() - 1));
            rNewTmp[0] = x0;
            rNewTmp[1] = y0;
            rNewTmp[2] = Math.max(1, x1 - x0);
            rNewTmp[3] = Math.max(1, y1 - y0);
        }
        invalidate();
    }

    /** 收尾：达标 → 建新件（选中、moved=true、id 由会话侧命名、**建完即收画笔**）；不达标 → 丢弃。 */
    void rNewFinish() {
        if (!rNewOn) return;
        rNewOn = false;
        boolean ok = rNewArm == 2 ? rNewTmp[2] >= BOX_MIN : (rNewTmp[2] >= BOX_MIN && rNewTmp[3] >= BOX_MIN);
        if (ok && bmp != null) {
            RegShape sh = new RegShape();
            sh.circle = rNewArm == 2;
            sh.moved = true;
            sh.g[0] = rNewTmp[0]; sh.g[1] = rNewTmp[1]; sh.g[2] = rNewTmp[2]; sh.g[3] = rNewTmp[3];
            rShapes.add(sh);
            rSel = rShapes.size() - 1;
            rNewArm = 0;                             /* 建完即收画笔（批量再点 [＋矩形]/[＋圆]） */
            notifyRegionGeom();
            notifyRegionSel();
        }
        invalidate();
    }

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
        if (bmp == null && spaceW <= 0) { zoom = clampF(z, ZOOM_MIN, ZOOM_MAX); return; }
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
        if (bmp != null || spaceW > 0) {
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

    private int bmpW() { return bmp != null ? bmp.getWidth() : spaceW; }
    private int bmpH() { return bmp != null ? bmp.getHeight() : spaceH; }

    private float fitScale() {
        if (bmp == null && spaceW <= 0) return 1f;
        int vw = Math.max(1, getWidth()), vh = Math.max(1, getHeight());
        return Math.min((float) vw / bmpW(), (float) vh / bmpH());
    }

    private float scale() { return fitScale() * zoom; }

    private void clampPan() {
        if (bmp == null && spaceW <= 0) { panX = panY = 0; return; }
        float s = scale();
        float cw = bmpW() * s, ch = bmpH() * s;
        int vw = getWidth(), vh = getHeight();
        float mx = vw * OVERPAN, my = vh * OVERPAN;
        if (cw <= vw) panX = (vw - cw) / 2f;
        else panX = Math.max(vw - cw - mx, Math.min(mx, panX));
        if (ch <= vh) panY = (vh - ch) / 2f;
        else panY = Math.max(vh - ch - my, Math.min(my, panY));
    }

    private void notifyBox() {
        if (listener != null) listener.onBox(sel[0], sel[1], sel[2], sel[3]);
    }

    private void notifyCircle() {
        if (circleListener != null) circleListener.onCircle(csel[0], csel[1], csel[2]);
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

    /* v12c 总览：给定矩形件的角柄命中（屏幕坐标；同 hitCorner 口径）。 */
    private int hitCornerOfRect(RegShape sh, float sx, float sy) {
        int r = Ui.dp(getContext(), HANDLE_DP);
        float s = scale();
        float x1 = panX + sh.g[0] * s, y1 = panY + sh.g[1] * s;
        float x2 = panX + (sh.g[0] + sh.g[2]) * s, y2 = panY + (sh.g[1] + sh.g[3]) * s;
        float[][] cs = {{x1, y1}, {x2, y1}, {x1, y2}, {x2, y2}};
        for (int i = 0; i < 4; i++)
            if (Math.hypot(sx - cs[i][0], sy - cs[i][1]) <= r) return i;
        return -1;
    }

    /* v12c 总览：按角增量缩放（同 resizeByCorner 口径，钳帧内与最小尺寸）。 */
    private void resizeShapeByCorner(RegShape sh, float dxS, float dyS) {
        float s = scale();
        int dx = Math.round(dxS / s), dy = Math.round(dyS / s);
        int bw = bmpW(), bh = bmpH();
        int x1 = sh.g[0], y1 = sh.g[1], x2 = sh.g[0] + sh.g[2], y2 = sh.g[1] + sh.g[3];
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
        sh.g[0] = x1; sh.g[1] = y1; sh.g[2] = x2 - x1; sh.g[3] = y2 - y1;
    }

    private void notifyRegionSel() {
        if (regionsListener != null) regionsListener.onRegionSelected(rSel);
    }

    private void notifyRegionGeom() {
        if (regionsListener != null) regionsListener.onRegionGeometryChanged();
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
        if (bmp == null && spaceW <= 0) return false;
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
                } else if (mode == MODE_CIRCLE) {    /* v12：圆环=调半径 / 圆内=挪圆心 / 圆外=新圆（按下定圆心） */
                    float s = scale();
                    float fx = (downX - panX) / s, fy = (downY - panY) / s;
                    float band = Ui.dp(getContext(), HANDLE_DP) / s;
                    if (cselOn) {
                        float dist = (float) Math.hypot(fx - csel[0], fy - csel[1]);
                        if (Math.abs(dist - csel[2]) <= band) grab = 5;
                        else if (dist <= csel[2]) grab = 6;
                        else {
                            grab = 7;
                            csel[0] = clampI(Math.round(fx), 0, bmpW());
                            csel[1] = clampI(Math.round(fy), 0, bmpH());
                            csel[2] = 0;
                            cselOn = true;
                        }
                    } else {
                        grab = 7;
                        csel[0] = clampI(Math.round(fx), 0, bmpW());
                        csel[1] = clampI(Math.round(fy), 0, bmpH());
                        csel[2] = 0;
                        cselOn = true;
                    }
                } else if (mode == MODE_REGIONS) {   /* v12c：先测选中件（角柄/环/体内）→ 再改选其它件（逆序=最上层）→ 空处=平移 */
                    /* v13：先测屏外边缘标记（点标记 = 选中该件） */
                    boolean markHit = false;
                    for (int k = offMarks.size() - 1; k >= 0; k--) {
                        float[] m = offMarks.get(k);
                        if (Math.abs(downX - m[0]) <= Ui.dp(getContext(), 26)
                                && Math.abs(downY - m[1]) <= Ui.dp(getContext(), 26)) {
                            rSel = (int) m[2];
                            notifyRegionSel();
                            markHit = true;
                            break;
                        }
                    }
                    if (markHit) {
                        grab = 0;
                        updateTouchTarget(e.getX(), e.getY());
                        invalidate();
                        return true;
                    }
                    float s = scale();
                    float fx = (downX - panX) / s, fy = (downY - panY) / s;
                    float band = Ui.dp(getContext(), HANDLE_DP) / s;
                    int g2 = 0;
                    if (rSel >= 0 && rSel < rShapes.size()) {
                        RegShape sh = rShapes.get(rSel);
                        if (sh.circle) {
                            float dist = (float) Math.hypot(fx - sh.g[0], fy - sh.g[1]);
                            if (Math.abs(dist - sh.g[2]) <= band) g2 = 22;
                            else if (dist <= sh.g[2]) g2 = 20;
                        } else {
                            int c = hitCornerOfRect(sh, downX, downY);
                            if (c >= 0) { g2 = 21; corner = c; }
                            else if (fx >= sh.g[0] && fy >= sh.g[1] && fx <= sh.g[0] + sh.g[2] && fy <= sh.g[1] + sh.g[3]) g2 = 20;
                        }
                    }
                    if (g2 == 0) {
                        int hit2 = -1;
                        for (int i = rShapes.size() - 1; i >= 0; i--) {
                            RegShape sh = rShapes.get(i);
                            boolean in = sh.circle
                                    ? Math.hypot(fx - sh.g[0], fy - sh.g[1]) <= sh.g[2]
                                    : (fx >= sh.g[0] && fy >= sh.g[1] && fx <= sh.g[0] + sh.g[2] && fy <= sh.g[1] + sh.g[3]);
                            if (in) { hit2 = i; break; }
                        }
                        if (hit2 != rSel) {
                            rSel = hit2;
                            notifyRegionSel();
                        }
                        if (hit2 >= 0) {
                            g2 = 20;
                            if (rNewArm != 0) { rNewArm = 0; notifyRegionSel(); }   /* v12d：点选已有 = 收画笔 */
                        } else if (rNewArm != 0) {
                            g2 = 30;                         /* v12d：空处起笔拖画新形状 */
                            rNewBegin(fx, fy);
                        }
                    }
                    grab = g2 != 0 ? g2 : 3;             /* 空处：点按=取消选择，拖动=平移 */
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
                } else if (mode == MODE_CIRCLE && grab == 5 && moved) {
                    float s = scale();
                    csel[2] = clampI((int) Math.round(Math.hypot((x - panX) / s - csel[0], (y - panY) / s - csel[1])),
                            0, Math.max(bmpW(), bmpH()));
                    notifyCircle();
                } else if (mode == MODE_CIRCLE && grab == 6 && moved) {
                    float s = scale();
                    csel[0] = clampI(csel[0] + Math.round(dx / s), 0, bmpW());
                    csel[1] = clampI(csel[1] + Math.round(dy / s), 0, bmpH());
                    notifyCircle();
                } else if (mode == MODE_CIRCLE && grab == 7 && moved) {
                    float s = scale();
                    csel[2] = clampI((int) Math.round(Math.hypot((x - panX) / s - csel[0], (y - panY) / s - csel[1])),
                            0, Math.max(bmpW(), bmpH()));
                    notifyCircle();
                } else if (mode == MODE_REGIONS && grab == 20 && moved && rSel >= 0 && rSel < rShapes.size()) {
                    float s = scale();
                    RegShape sh = rShapes.get(rSel);
                    if (sh.circle) {
                        sh.g[0] = clampI(sh.g[0] + Math.round(dx / s), 0, bmpW());
                        sh.g[1] = clampI(sh.g[1] + Math.round(dy / s), 0, bmpH());
                    } else {
                        sh.g[0] = clampI(sh.g[0] + Math.round(dx / s), 0, Math.max(0, bmpW() - sh.g[2]));
                        sh.g[1] = clampI(sh.g[1] + Math.round(dy / s), 0, Math.max(0, bmpH() - sh.g[3]));
                    }
                    sh.moved = true;
                    notifyRegionGeom();
                } else if (mode == MODE_REGIONS && grab == 21 && moved && rSel >= 0 && rSel < rShapes.size()) {
                    RegShape sh = rShapes.get(rSel);
                    resizeShapeByCorner(sh, dx, dy);
                    sh.moved = true;
                    notifyRegionGeom();
                } else if (mode == MODE_REGIONS && grab == 22 && moved && rSel >= 0 && rSel < rShapes.size()) {
                    float s = scale();
                    RegShape sh = rShapes.get(rSel);
                    sh.g[2] = clampI((int) Math.round(Math.hypot((x - panX) / s - sh.g[0], (y - panY) / s - sh.g[1])),
                            0, Math.max(bmpW(), bmpH()));
                    sh.moved = true;
                    notifyRegionGeom();
                } else if (mode == MODE_REGIONS && grab == 30 && moved) {   /* v12d：拖画新形状（只跟手，不动件表） */
                    float s = scale();
                    rNewMove((x - panX) / s, (y - panY) / s);
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
                if (mode == MODE_REGIONS && grab == 30) {   /* v12d：新形状收尾（不达标丢弃、画具保持） */
                    rNewFinish();
                }
                if (mode == MODE_CIRCLE && grab == 7) {   /* v12：新圆收尾（纯点按不建圆） */
                    if (!moved) {
                        cselOn = false;
                    } else {
                        if (csel[2] < BOX_MIN) csel[2] = BOX_MIN;
                        notifyCircle();
                    }
                    invalidate();
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
        if (bmp == null && spaceW <= 0) return;
        Context ctx = getContext();
        float s = scale();
        RectF content = new RectF(panX, panY, panX + bmpW() * s, panY + bmpH() * s);
        Paint p = new Paint();
        if (bmp != null && !bgHidden) {
            p.setFilterBitmap(zoom < 2f);
            c.drawBitmap(bmp, new Rect(0, 0, bmpW(), bmpH()), content, p);
        } else {
            /* v13：网格画布（无底图 / 底图隐藏） */
            p.setColor(0xFF0B0D0F);
            c.drawRect(content, p);
            Paint gl = new Paint();
            gl.setStrokeWidth(1);
            int g0 = Math.max(0, (int) ((0 - panX) / s)), g1 = Math.min(bmpW(), (int) ((getWidth() - panX) / s) + 1);
            int h0 = Math.max(0, (int) ((0 - panY) / s)), h1 = Math.min(bmpH(), (int) ((getHeight() - panY) / s) + 1);
            for (int x = (g0 / 100) * 100; x <= g1; x += 100) {
                float sx = panX + x * s;
                gl.setColor(x % 500 == 0 ? 0x2EFFFFFF : 0x14FFFFFF);
                c.drawLine(sx, content.top, sx, content.bottom, gl);
            }
            for (int y = (h0 / 100) * 100; y <= h1; y += 100) {
                float sy = panY + y * s;
                gl.setColor(y % 500 == 0 ? 0x2EFFFFFF : 0x14FFFFFF);
                c.drawLine(content.left, sy, content.right, sy, gl);
            }
            Paint bd = new Paint(Paint.ANTI_ALIAS_FLAG);
            bd.setStyle(Paint.Style.STROKE);
            bd.setStrokeWidth(Ui.dp(ctx, 1.5f));
            bd.setColor(0x66272D34);
            c.drawRect(content, bd);
        }
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
        // v12 圆形选区（遮罩 donut + 圆框 + 圆心/半径）
        if (cselOn && mode == MODE_CIRCLE) {
            float cxs = panX + csel[0] * s, cys = panY + csel[1] * s, rrs = csel[2] * s;
            Paint scrim2 = new Paint(Paint.ANTI_ALIAS_FLAG);
            scrim2.setColor(0x66000000);
            if (rrs <= 0.5f) {
                c.drawRect(0, 0, getWidth(), getHeight(), scrim2);
            } else {
                Path donut = new Path();
                donut.addRect(0, 0, getWidth(), getHeight(), Path.Direction.CW);
                Path hole = new Path();
                hole.addCircle(cxs, cys, rrs, Path.Direction.CCW);
                donut.op(hole, Path.Op.DIFFERENCE);
                c.drawPath(donut, scrim2);
            }
            Paint cp = new Paint(Paint.ANTI_ALIAS_FLAG);
            cp.setStyle(Paint.Style.STROKE);
            cp.setStrokeWidth(Ui.dp(ctx, 1.5f));
            cp.setColor(Ui.ACCENT);
            c.drawCircle(cxs, cys, Math.max(1f, rrs), cp);
            cp.setStrokeWidth(Ui.dp(ctx, 1));
            c.drawLine(cxs - Ui.dp(ctx, 12), cys, cxs + Ui.dp(ctx, 12), cys, cp);
            c.drawLine(cxs, cys - Ui.dp(ctx, 12), cxs, cys + Ui.dp(ctx, 12), cp);
            if (rrs > Ui.dp(ctx, 10)) {
                Paint rline = new Paint(Paint.ANTI_ALIAS_FLAG);
                rline.setColor(0x99FFFFFF);
                rline.setStrokeWidth(Ui.dp(ctx, 1));
                c.drawLine(cxs, cys, cxs + rrs, cys, rline);
                Paint tp = new Paint(Paint.ANTI_ALIAS_FLAG);
                tp.setColor(Ui.TEXT);
                tp.setTextSize(Ui.dp(ctx, 11));
                c.drawText("r=" + csel[2], cxs + rrs + Ui.dp(ctx, 6), cys - Ui.dp(ctx, 4), tp);
            }
        }
        // v12c 总览：全部区域（选中件最后画=最上；含 id 标签/角柄）——v13：裁到画面区 + 屏外边缘标记
        if (mode == MODE_REGIONS && !rShapes.isEmpty()) {
            RectF cr = new RectF(panX, panY, panX + bmpW() * s, panY + bmpH() * s);
            c.save();
            c.clipRect(cr);
            for (int i = 0; i < rShapes.size(); i++) {
                if (i != rSel) drawRegShape(c, ctx, s, rShapes.get(i), false);
            }
            if (rSel >= 0 && rSel < rShapes.size()) drawRegShape(c, ctx, s, rShapes.get(rSel), true);
            // v12d 拖画中的新形状（ghost；随裁）
            if (rNewOn) {
                rNewGhost.id = null;
                rNewGhost.circle = rNewArm == 2;
                rNewGhost.en = true;
                rNewGhost.g[0] = rNewTmp[0]; rNewGhost.g[1] = rNewTmp[1];
                rNewGhost.g[2] = rNewTmp[2]; rNewGhost.g[3] = rNewTmp[3];
                drawRegShape(c, ctx, s, rNewGhost, true);
            }
            c.restore();
            drawOffMarks(c, ctx, s, cr);
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

    /* v12c 总览：画一个区域件（描边+淡填充+id 标签；选中=琥珀高亮+角柄/半径手柄）。 */
    private void drawRegShape(Canvas c, Context ctx, float s, RegShape sh, boolean selected) {
        int strokeCol = selected ? Ui.ACCENT : (sh.en ? 0xFF9BA7B4 : 0xCCF07178);
        int fillCol = selected ? 0x30E8B14F : (sh.en ? 0x14000000 : 0x14F07178);
        Paint st = new Paint(Paint.ANTI_ALIAS_FLAG);
        st.setStyle(Paint.Style.STROKE);
        st.setStrokeWidth(Ui.dp(ctx, selected ? 2.5f : 1.5f));
        st.setColor(strokeCol);
        Paint fl = new Paint(Paint.ANTI_ALIAS_FLAG);
        fl.setColor(fillCol);
        if (sh.circle) {
            float cx = panX + sh.g[0] * s, cy = panY + sh.g[1] * s, r = Math.max(1f, sh.g[2] * s);
            c.drawCircle(cx, cy, r, fl);
            c.drawCircle(cx, cy, r, st);
            if (selected) {
                Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
                dot.setColor(Ui.ACCENT);
                c.drawCircle(cx, cy, Ui.dp(ctx, 3), dot);
                c.drawCircle(cx + r, cy, Ui.dp(ctx, 6), dot);       /* 半径手柄 */
            }
            drawRegLabel(c, ctx, sh, selected, cx, cy - r - Ui.dp(ctx, 6));
        } else {
            float x1 = panX + sh.g[0] * s, y1 = panY + sh.g[1] * s;
            float x2 = panX + (sh.g[0] + sh.g[2]) * s, y2 = panY + (sh.g[1] + sh.g[3]) * s;
            c.drawRect(new RectF(x1, y1, x2, y2), fl);
            c.drawRect(new RectF(x1, y1, x2, y2), st);
            if (selected) {
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
            drawRegLabel(c, ctx, sh, selected, x1, y1 - Ui.dp(ctx, 4));
        }
    }

    /* v12c 总览：id 标签（深底小签；y = 标签底边）。 */
    private void drawRegLabel(Canvas c, Context ctx, RegShape sh, boolean selected, float x, float y) {
        Paint tp = new Paint(Paint.ANTI_ALIAS_FLAG);
        tp.setTextSize(Ui.dp(ctx, 11));
        tp.setColor(selected ? Ui.ACCENT : (sh.en ? Ui.TEXT : 0xFFF07178));
        if (sh.id == null) return;                   /* 未命名（拖画中的 ghost / 新件待命名）不打扰 */
        String id = sh.id;
        float w = tp.measureText(id);
        float h = Ui.dp(ctx, 14);
        Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
        bg.setColor(0xCC0C0E10);
        float bx = x, by = y - h;
        c.drawRoundRect(new RectF(bx, by, bx + w + Ui.dp(ctx, 8), by + h), Ui.dp(ctx, 3), Ui.dp(ctx, 3), bg);
        c.drawText(id, bx + Ui.dp(ctx, 4), by + h - Ui.dp(ctx, 3), tp);
    }

    /* v13：屏外区域边缘标记（完全在画面区外的件在最近边画圆点 + id；可点按选中）。 */
    private void drawOffMarks(Canvas c, Context ctx, float s, RectF cr) {
        offMarks.clear();
        int W = bmpW(), H = bmpH();
        for (int i = 0; i < rShapes.size(); i++) {
            RegShape sh = rShapes.get(i);
            boolean out = sh.circle
                    ? (sh.g[0] + sh.g[2] <= 0 || sh.g[0] - sh.g[2] >= W || sh.g[1] + sh.g[2] <= 0 || sh.g[1] - sh.g[2] >= H)
                    : (sh.g[0] + sh.g[2] <= 0 || sh.g[0] >= W || sh.g[1] + sh.g[3] <= 0 || sh.g[1] >= H);
            if (!out) continue;
            int cx = sh.circle ? sh.g[0] : sh.g[0] + sh.g[2] / 2;
            int cy = sh.circle ? sh.g[1] : sh.g[1] + sh.g[3] / 2;
            float mx = panX + clampI(cx, 0, W) * s, my = panY + clampI(cy, 0, H) * s;
            mx = clampF(mx, cr.left + Ui.dp(ctx, 14), cr.right - Ui.dp(ctx, 14));
            my = clampF(my, cr.top + Ui.dp(ctx, 14), cr.bottom - Ui.dp(ctx, 14));
            boolean sel = i == rSel;
            Paint pt = new Paint(Paint.ANTI_ALIAS_FLAG);
            pt.setColor(sel ? Ui.ACCENT : (sh.en ? 0xFF9BA7B4 : 0xCCF07178));
            c.drawCircle(mx, my, Ui.dp(ctx, 5), pt);
            Paint tp2 = new Paint(Paint.ANTI_ALIAS_FLAG);
            tp2.setTextSize(Ui.dp(ctx, 11));
            tp2.setColor(sel ? Ui.ACCENT : (sh.en ? Ui.TEXT : 0xFFF07178));
            if (sh.id != null) c.drawText(sh.id, mx + Ui.dp(ctx, 8), my + Ui.dp(ctx, 4), tp2);
            offMarks.add(new float[]{mx, my, i});
        }
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
