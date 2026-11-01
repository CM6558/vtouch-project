package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * 脚本干跑（编辑悬浮窗「脚本测试」）：在给定帧上评估操作各「视觉步」的命中情况
 * （找图 / 找色单点 / 找色多点；含步骤自带的阈值、色校、区域限定），不执行任何触摸。
 * 区域限定口径同 OpsPage.regionRect：矩形取 (a1,a2,a3,a4)，圆取外接矩形，均按竖屏逻辑矩形处理。
 */
public final class DryRun {
    public static final class Line {
        public int no;
        public int type;
        public boolean hit;
        public String text = "";
        public int x, y, w, h;
    }

    public static final class Report {
        public String opName = "";
        public final List<Line> lines = new ArrayList<Line>();
        public int visual, hits, misses, others;
        public String summary = "";
    }

    public static Report run(byte[] rgba, int w, int h, int rot, int baseRot, int baseW, int baseH,
                             Ops.Op op, Map<String, int[]> regions) {
        Report rep = new Report();
        rep.opName = op.name;
        Matcher.Frame f = Matcher.prepare(rgba, w, h, rot);
        if (f == null) {
            rep.summary = "画面准备失败";
            return rep;
        }
        int no = 0;
        for (Ops.Step st : op.steps) {
            no++;
            if (st.type != 10 && st.type != 11) {
                rep.others++;
                continue;
            }
            Line ln = new Line();
            ln.no = no;
            ln.type = st.type;
            rep.visual++;
            ln.text = st.type == 10
                    ? dryFindImage(f, w, h, rot, baseRot, baseW, baseH, st, regions, ln)
                    : dryFindColor(f, w, h, rot, baseRot, baseW, baseH, st, regions, ln);
            if (ln.hit) rep.hits++;
            else rep.misses++;
            rep.lines.add(ln);
        }
        rep.summary = op.name + "：视觉步 " + rep.visual + " · 命中 " + rep.hits + " · 未命中 " + rep.misses
                + (rep.others > 0 ? " · 其余 " + rep.others + " 步需运行时" : "");
        return rep;
    }

    /** 区域矩形 → 帧坐标：按**基帧**（#frame）换算——与面板/核心口径一致（v12e）。 */
    private static int[] frameRegion(int baseRot, int baseW, int baseH, String regionName, Map<String, int[]> regions) {
        if (regionName == null || regionName.isEmpty() || regions == null) return null;
        int[] lr = regions.get(regionName);
        if (lr == null || lr.length < 4) return null;
        return Matcher.logicRectToFrame(baseRot, baseW, baseH, lr[0], lr[1], lr[2], lr[3]);
    }

    private static String dryFindImage(Matcher.Frame f, int w, int h, int rot, int baseRot, int baseW, int baseH,
                                       Ops.Step st, Map<String, int[]> regions, Line ln) {
        String tag = "找图 " + st.ref + "（阈值 " + st.a1 + (st.a2 == 1 ? "·色校" : "") + "）";
        Assets.Tmpl t = Assets.readTmpl(st.ref);
        if (t == null) return tag + " → 模板读不到";
        byte[] gray = t.gray;
        int tw = t.w, th = t.h;
        int steps = (t.rot - rot) & 3;
        if (steps != 0) {
            int[] nwh = new int[2];
            byte[] g2 = Matcher.rotateGray(gray, tw, th, steps, nwh);
            if (g2 == null) return tag + " → 模板旋转失败";
            gray = g2;
            tw = nwh[0];
            th = nwh[1];
        }
        int rx = 0, ry = 0, rw = w, rh = h;
        String rs = "";
        int[] fr = frameRegion(baseRot, baseW, baseH, st.expr, regions);
        if (fr != null) {
            rx = fr[0]; ry = fr[1]; rw = fr[2]; rh = fr[3];
            rs = " 区域:" + st.expr;
        }
        Matcher.Hit hit = Matcher.findImage(f, gray, tw, th, Math.max(0, st.a1), rx, ry, rw, rh);
        if (hit != null && st.a2 == 1 && t.avgRgb >= 0
                && !Matcher.colorCheck(f, hit.x, hit.y, tw, th, t.avgRgb)) {
            hit = null;
        }
        if (hit == null) return tag + rs + " → 未命中";
        ln.hit = true;
        ln.x = hit.x; ln.y = hit.y; ln.w = tw; ln.h = th;
        return tag + rs + " → 命中 (" + hit.x + "," + hit.y + ")";
    }

    private static String dryFindColor(Matcher.Frame f, int w, int h, int rot, int baseRot, int baseW, int baseH,
                                       Ops.Step st, Map<String, int[]> regions, Line ln) {
        int rx = 0, ry = 0, rw = w, rh = h;
        String rs = "";
        int[] fr = frameRegion(baseRot, baseW, baseH, st.expr, regions);
        if (fr != null) {
            rx = fr[0]; ry = fr[1]; rw = fr[2]; rh = fr[3];
            rs = " 区域:" + st.expr;
        }
        if (st.a1 == 1) {                      /* 多点（点集） */
            String tag = "找色多点 " + st.ref;
            Assets.Pts p = Assets.readPts(st.ref);
            if (p == null || p.pts == null || p.pts.length == 0) return tag + " → 点集读不到";
            Matcher.Pt[] pts = new Matcher.Pt[p.pts.length];
            for (int i = 0; i < pts.length; i++) {
                Matcher.Pt q = new Matcher.Pt();
                q.dx = p.pts[i].dx;
                q.dy = p.pts[i].dy;
                q.rgb = p.pts[i].rgb;
                q.tol = p.pts[i].tol;
                pts[i] = q;
            }
            Matcher.Hit hit = Matcher.findColorMulti(f, rx, ry, rw, rh, p.baseRgb, p.baseTol, pts, pts.length);
            if (hit == null) return tag + rs + " → 未命中";
            ln.hit = true;
            ln.x = hit.x; ln.y = hit.y; ln.w = 24; ln.h = 24;
            return tag + rs + " → 命中 (" + hit.x + "," + hit.y + ")";
        }
        int rgb = (st.a2 >>> 8) & 0xFFFFFF;
        int tol = st.a2 & 0xFF;
        String tag = String.format(Locale.US, "找色 #%06X 容差%d", rgb, tol);
        Matcher.Hit hit = Matcher.findColor(f, rx, ry, rw, rh, rgb, tol);
        if (hit == null) return tag + rs + " → 未命中";
        ln.hit = true;
        ln.x = hit.x; ln.y = hit.y; ln.w = 16; ln.h = 16;
        return tag + rs + " → 命中 (" + hit.x + "," + hit.y + ")";
    }
}
