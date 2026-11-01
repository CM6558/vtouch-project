package com.vtouch.editor;

/** 离机回归：找色轻量帧与完整帧的结果一致，且不会提前分配灰度金字塔。 */
public final class MatcherColorCheck {
    private static void check(boolean ok, String label) {
        if (!ok) throw new AssertionError(label);
    }

    private static boolean same(Matcher.Hit a, Matcher.Hit b) {
        return a == null ? b == null : b != null && a.x == b.x && a.y == b.y;
    }

    public static void main(String[] args) {
        int w = 47, h = 39;
        byte[] rgba = new byte[w * h * 4];
        for (int i = 0; i < w * h; i++) {
            int p = i * 4;
            rgba[p] = (byte) (i * 31);
            rgba[p + 1] = (byte) (i * 17);
            rgba[p + 2] = (byte) (i * 7);
            rgba[p + 3] = (byte) 255;
        }
        Matcher.Frame color = Matcher.prepareColor(rgba, w, h, 5);
        Matcher.Frame full = Matcher.prepare(rgba, w, h, 5);
        check(color != null && full != null && color.rgba == rgba && color.rot == full.rot,
                "轻量帧元数据");
        check(color.gray == null && color.gray2 == null && color.gray4 == null,
                "轻量帧未分配灰度金字塔");
        check(full.gray != null && full.gray2 != null && full.gray4 != null,
                "完整帧保留找图支持");
        byte[] pixel = {full.gray[0]};
        check(same(Matcher.findImage(full, pixel, 1, 1, 0, 0, 0, 1, 1), new Matcher.Hit(0, 0)),
                "找图仍可使用完整帧");
        check(Matcher.prepareColor(rgba, w + 1, h, 0) == null
                && Matcher.prepareColor(null, w, h, 0) == null,
                "帧输入校验");

        Matcher.Pt pt = new Matcher.Pt();
        pt.dx = 1; pt.dy = 0; pt.rgb = 0x123456; pt.tol = 255;
        Matcher.Pt[] pts = {pt};
        Matcher.Hit known = Matcher.findColor(color, 0, 0, w, h, 0x000000, 0);
        check(known != null && known.x == 0 && known.y == 0, "轻量帧实际命中");
        int[] regions = {0, 0, w, h, 10, 8, 20, 16, -8, -8, 11, 11};
        for (int i = 0; i < regions.length; i += 4) {
            for (int tol : new int[]{0, 12, 255}) {
                int rgb = 0x78A0C0;
                check(same(Matcher.findColor(color, regions[i], regions[i + 1], regions[i + 2], regions[i + 3], rgb, tol),
                        Matcher.findColor(full, regions[i], regions[i + 1], regions[i + 2], regions[i + 3], rgb, tol)),
                        "单点找色结果");
                check(same(Matcher.findColorMulti(color, regions[i], regions[i + 1], regions[i + 2], regions[i + 3],
                                rgb, tol, pts, 1),
                        Matcher.findColorMulti(full, regions[i], regions[i + 1], regions[i + 2], regions[i + 3],
                                rgb, tol, pts, 1)), "多点找色结果");
            }
        }
        check(Matcher.colorCheck(color, 4, 5, 3, 3, 0x123456)
                == Matcher.colorCheck(full, 4, 5, 3, 3, 0x123456), "颜色校验结果");
        System.out.println("MatcherColorCheck OK");
    }
}
