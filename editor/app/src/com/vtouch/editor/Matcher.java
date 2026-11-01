package com.vtouch.editor;

/**
 * 匹配引擎 —— src/vt_vision.c 标量路径的 Java 直译（本地「试一下」用；对拍门见 v7 spec §6 P1a）。
 * 直译自：vis_gray_scalar:134-145 / vis_down2:159-171 / vis_sad_ok:388-403 / vis_image_direct:416-432 /
 * vis_image_pyramid:452-505（粗筛余量 VT_VIS_COARSE_MARGIN=8、中心裁剪 +tw4+1、flag 胞行、全局行优先精修）/
 * vis_px_match:183-193 / vis_color_row_scalar:248-255 / vis_pts_ok:272-286 / vis_clip_span:298-308 /
 * vt_vis_find_color:557-575 / vt_vis_find_color_multi:589-621 / vt_vis_find_image:634-651 /
 * vt_vis_frame_to_logic:664-678 / vt_vis_logic_rect_to_frame:689-701；模板旋转照 vt_ops.c op_vis_rot_gray:1154-1173
 * （steps = (trot - fr.rot) & 3 顺时针步）；颜色校验照 vt_ops.c:1254-1283（VT_OPS_COLOR_TOL=48）。
 * 语义：命中判据 SAD ≤ thresh×tw×th（含等号）；行优先命中即停；金字塔为启发式（细小纹理允许漏检，口径同 C）。
 * 备注：四邻读的边界由与 C 相同的守卫（2*X2 > xe 等）保证；数组尾部留少量余量垫片防越界（值层面同 C 语义）。
 */
public final class Matcher {
    public static final int COARSE_MARGIN = 8;   /* VT_VIS_COARSE_MARGIN */
    public static final int MAX_W = 4096;
    public static final int MAX_H = 4096;
    public static final int PT_MAX = 4096;
    public static final int COLOR_TOL = 48;      /* VT_OPS_COLOR_TOL */

    public static final class Frame { public byte[] rgba; public int w, h, rot; public byte[] gray, gray2, gray4; }
    public static final class Hit { public final int x, y; public Hit(int x, int y) { this.x = x; this.y = y; } }
    public static final class Pt { public int dx, dy, rgb, tol; }

    /** 灰度公式（唯一来源同 vt_vision.h）：(77r+150g+29b+128)>>8。 */
    public static byte grayPx(int r, int g, int b) {
        return (byte) ((77 * r + 150 * g + 29 * b + 128) >> 8);
    }

    /** 仅找色时的帧视图：复用 RGBA，不扫描整帧或分配灰度金字塔。 */
    public static Frame prepareColor(byte[] rgba, int w, int h, int rot) {
        if (rgba == null || w <= 0 || h <= 0 || w > MAX_W || h > MAX_H) return null;
        if (rgba.length < w * h * 4) return null;
        Frame f = new Frame();
        f.rgba = rgba; f.w = w; f.h = h; f.rot = rot & 3;
        return f;
    }

    /** 帧视图：RGBA → 灰度 + 1/2、1/4 盒式金字塔（对应 vt_vis_frame_prepare）。 */
    public static Frame prepare(byte[] rgba, int w, int h, int rot) {
        Frame f = prepareColor(rgba, w, h, rot);
        if (f == null) return null;
        f.gray = new byte[w * h];
        for (int y = 0; y < h; y++) {
            int p = y * w * 4, d = y * w;
            for (int x = 0; x < w; x++, p += 4)
                f.gray[d + x] = grayPx(rgba[p] & 0xFF, rgba[p + 1] & 0xFF, rgba[p + 2] & 0xFF);
        }
        int w2 = w / 2, h2 = h / 2, w4 = w / 4, h4 = h / 4;
        f.gray2 = down2(f.gray, w, h, w2, h2);
        f.gray4 = down2(f.gray2, w2, h2, w4, h4);
        return f;
    }

    /** 1/2 盒式下采样（四舍五入 (a+b+c+d+2)>>2；奇数边尾部不参与）——vis_down2 直译。 */
    static byte[] down2(byte[] src, int sw, int sh, int dw, int dh) {
        byte[] d = new byte[dw * dh + dw + 8];   /* 尾部垫片：防极端几何下的越界读（值语义同 C 的静态大缓冲） */
        for (int y = 0; y < dh; y++) {
            int r0 = (2 * y) * sw, r1 = r0 + sw, o = y * dw;
            for (int x = 0; x < dw; x++)
                d[o + x] = (byte) (((src[r0 + 2 * x] & 0xFF) + (src[r0 + 2 * x + 1] & 0xFF)
                        + (src[r1 + 2 * x] & 0xFF) + (src[r1 + 2 * x + 1] & 0xFF) + 2) >> 2);
        }
        return d;
    }

    /** SAD 行级早退（SSDA）——vis_sad_ok 直译；tOff = 模板视图起始偏移（中心裁剪用）。 */
    static boolean sadOk(byte[] f, int fw, int x, int y, byte[] t, int tOff, int tstride, int tw, int th, long limit) {
        long sad = 0;
        int fp = y * fw + x, tp = tOff;
        for (int i = 0; i < th; i++) {
            long rs = 0;
            for (int k = 0; k < tw; k++) {
                int a = f[fp + k] & 0xFF, b = t[tp + k] & 0xFF;
                rs += a > b ? a - b : b - a;
            }
            sad += rs;
            if (sad > limit) return false;
            fp += fw; tp += tstride;
        }
        return true;
    }

    /** [v0, v0+n-1] ∩ [0, lim-1]；空返回 null —— vis_clip_span 直译。 */
    static int[] clip(int v0, int n, int lim) {
        int a = v0, b = v0 + n - 1;
        if (a < 0) a = 0;
        if (b > lim - 1) b = lim - 1;
        if (b < a) return null;
        return new int[]{a, b};
    }

    /** 全分辨率直搜（模板 <8 的回退路径）——vis_image_direct 直译。 */
    static Hit imageDirect(Frame f, int x0, int x1, int y0, int y1, byte[] tmpl, int tw, int th, long limit) {
        int xe = x1 - tw + 1, ye = y1 - th + 1;
        for (int y = y0; y <= ye; y++)
            for (int x = x0; x <= xe; x++)
                if (sadOk(f.gray, f.w, x, y, tmpl, 0, tw, tw, th, limit)) return new Hit(x, y);
        return null;
    }

    /** 金字塔：1/4 粗筛 → 1/2 定位 → 全分辨率精修 —— vis_image_pyramid 直译。 */
    static Hit imagePyramid(Frame f, int x0, int x1, int y0, int y1, byte[] tmpl, int tw, int th, long limit, int thresh) {
        int tw2 = tw / 2, th2 = th / 2, tw4 = tw / 4, th4 = th / 4;
        int tw2c = tw2 - 1, th2c = th2 - 1;
        int tw4c = tw4 - 1, th4c = th4 - 1;
        long lim4 = (long) (thresh + COARSE_MARGIN) * tw4c * th4c;
        long lim2 = (long) (thresh + COARSE_MARGIN) * tw2c * th2c;
        int xe = x1 - tw + 1, ye = y1 - th + 1;
        int X0 = x0 / 4, X1 = xe / 4;
        int w2 = f.w / 2, w4 = f.w / 4;
        byte[] t2 = down2(tmpl, tw, th, tw2, th2);
        byte[] t4 = down2(t2, tw2, th2, tw4, th4);
        byte[] flagrow = new byte[f.w / 4 + 2];
        int flagY = -1;
        boolean rowAny = false;
        for (int y = y0; y <= ye; y++) {
            int Y = y / 4, Y2 = y / 2;
            if (Y != flagY) {
                flagY = Y;
                rowAny = false;
                for (int X = X0; X <= X1; X++) {
                    boolean ok = sadOk(f.gray4, w4, X + 1, Y + 1, t4, tw4 + 1, tw4, tw4c, th4c, lim4);
                    flagrow[X] = (byte) (ok ? 1 : 0);
                    if (ok) rowAny = true;
                }
            }
            if (!rowAny) continue;
            for (int X = X0; X <= X1; X++) {
                if (flagrow[X] == 0) continue;
                for (int X2 = 2 * X; X2 <= 2 * X + 1; X2++) {
                    if (2 * X2 > xe || 2 * X2 + 1 < x0) continue;
                    if (!sadOk(f.gray2, w2, X2 + 1, Y2 + 1, t2, tw2 + 1, tw2, tw2c, th2c, lim2)) continue;
                    int xa = 2 * X2;
                    if (xa < x0) xa = x0;
                    int xb = 2 * X2 + 1;
                    if (xb > xe) xb = xe;
                    for (int x = xa; x <= xb; x++)
                        if (sadOk(f.gray, f.w, x, y, tmpl, 0, tw, tw, th, limit)) return new Hit(x, y);
                }
            }
        }
        return null;
    }

    /** 找图：命中判据 SAD ≤ thresh×tw×th —— vt_vis_find_image 直译。 */
    public static Hit findImage(Frame f, byte[] tmpl, int tw, int th, int thresh, int rx, int ry, int rw, int rh) {
        if (f == null || f.gray == null || tmpl == null) return null;
        if (tw <= 0 || th <= 0 || tw > f.w || th > f.h || tw > MAX_W || th > MAX_H) return null;
        if (thresh < 0 || thresh > 255 || rw <= 0 || rh <= 0) return null;
        int[] xs = clip(rx, rw, f.w), ys = clip(ry, rh, f.h);
        if (xs == null || ys == null) return null;
        int x0 = xs[0], x1 = xs[1], y0 = ys[0], y1 = ys[1];
        if (x1 - x0 + 1 < tw || y1 - y0 + 1 < th) return null;
        long limit = (long) thresh * tw * th;
        if (tw >= 8 && th >= 8) return imagePyramid(f, x0, x1, y0, y1, tmpl, tw, th, limit, thresh);
        return imageDirect(f, x0, x1, y0, y1, tmpl, tw, th, limit);
    }

    /** 单像素颜色匹配：per-channel max-diff ≤ tol（只比 R/G/B）——vis_px_match 直译。 */
    static boolean pxMatch(byte[] rgba, int p, int tr, int tg, int tb, int tol) {
        int dr = (rgba[p] & 0xFF) - tr; if (dr < 0) dr = -dr;
        int dg = (rgba[p + 1] & 0xFF) - tg; if (dg < 0) dg = -dg;
        int db = (rgba[p + 2] & 0xFF) - tb; if (db < 0) db = -db;
        return dr <= tol && dg <= tol && db <= tol;
    }

    /** 找色：区域行优先找首个匹配 —— vt_vis_find_color 直译。 */
    public static Hit findColor(Frame f, int rx, int ry, int rw, int rh, int rgb, int tol) {
        if (f == null || f.rgba == null) return null;
        if (tol < 0 || tol > 255 || rw <= 0 || rh <= 0) return null;
        int[] xs = clip(rx, rw, f.w), ys = clip(ry, rh, f.h);
        if (xs == null || ys == null) return null;
        int tr = (rgb >> 16) & 0xFF, tg = (rgb >> 8) & 0xFF, tb = rgb & 0xFF;
        for (int y = ys[0]; y <= ys[1]; y++) {
            int base = y * f.w * 4;
            for (int x = xs[0]; x <= xs[1]; x++)
                if (pxMatch(f.rgba, base + x * 4, tr, tg, tb, tol)) return new Hit(x, y);
        }
        return null;
    }

    /** 参考点全验（基准 + 偏移，越界=该候选不成立）——vis_pts_ok 直译。 */
    static boolean ptsOk(Frame f, int bx, int by, Pt[] pts, int n) {
        for (int i = 0; i < n; i++) {
            int px = bx + pts[i].dx, py = by + pts[i].dy;
            if (px < 0 || px >= f.w || py < 0 || py >= f.h) return false;
            if (!pxMatch(f.rgba, (py * f.w + px) * 4,
                    (pts[i].rgb >> 16) & 0xFF, (pts[i].rgb >> 8) & 0xFF, pts[i].rgb & 0xFF, pts[i].tol))
                return false;
        }
        return true;
    }

    /** 多点找色：基准命中后逐点校验 —— vt_vis_find_color_multi 直译。 */
    public static Hit findColorMulti(Frame f, int rx, int ry, int rw, int rh, int baseRgb, int baseTol, Pt[] pts, int n) {
        if (f == null || f.rgba == null || pts == null) return null;
        if (baseTol < 0 || baseTol > 255 || n < 1 || n > 16 || rw <= 0 || rh <= 0) return null;
        for (int i = 0; i < n; i++) {
            if (pts[i] == null || pts[i].tol < 0 || pts[i].tol > 255) return null;
            if (pts[i].dx < -PT_MAX || pts[i].dx > PT_MAX || pts[i].dy < -PT_MAX || pts[i].dy > PT_MAX) return null;
        }
        int[] xs = clip(rx, rw, f.w), ys = clip(ry, rh, f.h);
        if (xs == null || ys == null) return null;
        int tr = (baseRgb >> 16) & 0xFF, tg = (baseRgb >> 8) & 0xFF, tb = baseRgb & 0xFF;
        for (int y = ys[0]; y <= ys[1]; y++) {
            int base = y * f.w * 4;
            int x = xs[0];
            while (x <= xs[1]) {
                int hx = -1;
                for (int xx = x; xx <= xs[1]; xx++) {
                    if (pxMatch(f.rgba, base + xx * 4, tr, tg, tb, baseTol)) { hx = xx; break; }
                }
                if (hx < 0) break;
                if (ptsOk(f, hx, y, pts, n)) return new Hit(hx, y);
                x = hx + 1;
            }
        }
        return null;
    }

    /** 颜色校验：命中区域平均色逐通道 ≤48（无存色 avgRgb<0 → 跳过=true）——vt_ops.c:1254-1283 直译。 */
    public static boolean colorCheck(Frame f, int x, int y, int tw, int th, int avgRgb) {
        if (avgRgb < 0) return true;
        if (f == null || f.rgba == null || tw < 1 || th < 1) return false;
        if (x < 0 || y < 0 || x + tw > f.w || y + th > f.h) return false;
        long sr = 0, sg = 0, sb = 0;
        for (int yy = 0; yy < th; yy++) {
            int row = ((y + yy) * f.w + x) * 4;
            for (int xx = 0; xx < tw; xx++) {
                int p = row + xx * 4;
                sr += f.rgba[p] & 0xFF; sg += f.rgba[p + 1] & 0xFF; sb += f.rgba[p + 2] & 0xFF;
            }
        }
        long n = (long) tw * th;
        int ar = (int) (sr / n), ag = (int) (sg / n), ab = (int) (sb / n);
        int er = (avgRgb >> 16) & 0xFF, eg = (avgRgb >> 8) & 0xFF, eb = avgRgb & 0xFF;
        return Math.abs(ar - er) <= COLOR_TOL && Math.abs(ag - eg) <= COLOR_TOL && Math.abs(ab - eb) <= COLOR_TOL;
    }

    /** 模板方向对齐：顺时针 steps 步（调用方保证 steps∈{1,2,3}）——op_vis_rot_gray 直译。 */
    public static byte[] rotateGray(byte[] src, int sw, int sh, int steps, int[] outWH) {
        if (src == null || sw < 1 || sh < 1) return null;
        steps &= 3;
        int ow, oh;
        if (steps == 2) { ow = sw; oh = sh; } else { ow = sh; oh = sw; }
        byte[] dst = new byte[ow * oh];
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                int sx, sy;
                if (steps == 1) { sx = y; sy = sh - 1 - x; }
                else if (steps == 2) { sx = sw - 1 - x; sy = sh - 1 - y; }
                else { sx = sw - 1 - y; sy = x; }
                dst[y * ow + x] = src[sy * sw + sx];
            }
        }
        if (outWH != null) { outWH[0] = ow; outWH[1] = oh; }
        return dst;
    }

    /** 帧坐标（当前方向）→ 竖屏逻辑坐标（单点）——vt_vis_frame_to_logic 直译。 */
    public static void frameToLogic(int rot, int fw, int fh, int fx, int fy, int[] out2) {
        rot &= 3;
        int W, H;
        if (rot == 1 || rot == 3) { W = fh; H = fw; } else { W = fw; H = fh; }
        int lx, ly;
        switch (rot) {
            case 1: lx = W - 1 - fy; ly = fx; break;
            case 2: lx = W - 1 - fx; ly = H - 1 - fy; break;
            case 3: lx = fy; ly = H - 1 - fx; break;
            default: lx = fx; ly = fy; break;
        }
        out2[0] = lx; out2[1] = ly;
    }

    /** 竖屏逻辑矩形 → 帧坐标矩形 {fx,fy,fw2,fh2}——vt_vis_logic_rect_to_frame 直译。 */
    public static int[] logicRectToFrame(int rot, int fw, int fh, int lx, int ly, int lw, int lh) {
        rot &= 3;
        int fx, fy, w2, h2;
        switch (rot) {
            case 1: fx = ly; fy = fh - lx - lw; w2 = lh; h2 = lw; break;
            case 2: fx = fw - lx - lw; fy = fh - ly - lh; w2 = lw; h2 = lh; break;
            case 3: fx = fw - ly - lh; fy = lx; w2 = lh; h2 = lw; break;
            default: fx = lx; fy = ly; w2 = lw; h2 = lh; break;
        }
        return new int[]{fx, fy, w2, h2};
    }
}
