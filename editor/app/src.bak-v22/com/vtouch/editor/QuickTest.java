package com.vtouch.editor;

/**
 * 悬浮窗「试一下」：在给定帧上跑一次素材查找（模板 / 点集；全屏区域；找图阈值由调用方给）。
 * 模板方向差自动旋正（同核心口径 steps=(trot-fr.rot)&3）；命中坐标 = 帧坐标（当前方向）。
 */
public final class QuickTest {
    public static final class Result {
        public boolean hit;
        public int x, y, w, h;
        public String text;
    }

    public static Result run(byte[] rgba, int w, int h, int rot, String name, boolean isPts, int thresh) {
        Result r = new Result();
        if (rgba == null || name == null || name.isEmpty()) {
            r.text = "参数不完整";
            return r;
        }
        Matcher.Frame f = Matcher.prepare(rgba, w, h, rot);
        if (f == null) {
            r.text = "画面准备失败";
            return r;
        }
        if (!isPts) {
            Assets.Tmpl t = Assets.readTmpl(name);
            if (t == null) {
                r.text = "模板读不到：" + name;
                return r;
            }
            byte[] gray = t.gray;
            int tw = t.w, th = t.h;
            int steps = (t.rot - rot) & 3;
            if (steps != 0) {
                int[] nwh = new int[2];
                byte[] g2 = Matcher.rotateGray(gray, tw, th, steps, nwh);
                if (g2 == null) {
                    r.text = "模板旋转失败";
                    return r;
                }
                gray = g2;
                tw = nwh[0];
                th = nwh[1];
            }
            long t0 = System.currentTimeMillis();
            Matcher.Hit hit = Matcher.findImage(f, gray, tw, th, thresh, 0, 0, w, h);
            long dt = System.currentTimeMillis() - t0;
            if (hit == null) {
                r.text = "未命中（阈值 " + thresh + "）· " + dt + "ms";
                return r;
            }
            r.hit = true;
            r.x = hit.x;
            r.y = hit.y;
            r.w = tw;
            r.h = th;
            r.text = "命中 (" + hit.x + "," + hit.y + ")（阈值 " + thresh + "）· " + dt + "ms";
        } else {
            Assets.Pts p = Assets.readPts(name);
            if (p == null || p.pts == null || p.pts.length == 0) {
                r.text = "点集读不到：" + name;
                return r;
            }
            Matcher.Pt[] pts = new Matcher.Pt[p.pts.length];
            for (int i = 0; i < pts.length; i++) {
                Matcher.Pt q = new Matcher.Pt();
                q.dx = p.pts[i].dx;
                q.dy = p.pts[i].dy;
                q.rgb = p.pts[i].rgb;
                q.tol = p.pts[i].tol;
                pts[i] = q;
            }
            long t0 = System.currentTimeMillis();
            Matcher.Hit hit = Matcher.findColorMulti(f, 0, 0, w, h, p.baseRgb, p.baseTol, pts, pts.length);
            long dt = System.currentTimeMillis() - t0;
            if (hit == null) {
                r.text = "未命中 · " + dt + "ms";
                return r;
            }
            r.hit = true;
            r.x = hit.x;
            r.y = hit.y;
            r.w = 24;
            r.h = 24;
            r.text = "命中 (" + hit.x + "," + hit.y + ") · " + dt + "ms";
        }
        return r;
    }
}
