package com.vtouch.editor;

import android.graphics.Bitmap;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * 素材文件层（.tmpl / .pts）——格式逐字节镜像面板写端（参照 vtouch_ui.cpp:7020-7022、:7275-7277、
 * 读端 :7082-7115 / :7172-7216）。落盘目录 /data/local/vtouch-runtime/templates/（核心运行期直接读）。
 *
 * .tmpl = "VTM1" + ver u32=1 + w u16 + h u16 + rot u8 + res u8=0 + gray[w*h]
 *         + 尾扩展 x u16 + y u16 + 平均色 r/g/b u8×3 + res u8（总是写 8B；读端 ≥4B 认 x/y、≥8B 认平均色）。
 * .pts  = "VTP1" + ver u32=1 + n u16 + res u16=0 + base_rgb u32 + base_tol u16
 *         + n×{dx i16, dy i16, rgb u32, tol u16} + 尾扩展 x u16 + y u16（基准位置）。
 * 灰度 = (77r+150g+29b+128)>>8（唯一来源 vt_vision.h 的 vt_vis_gray_px）；平均色 = 逐通道和 ÷ 像素数（取整）。
 * 命名 v11（可见字符 1..15、可用中文；裸 "-" 拒收）、.tmpl/.pts 共用名字空间。dir 可重定向（测试 scratch）。
 */
public final class Assets {
    public static String dir = Ops.TMPL_DIR;

    public static final class Tmpl { public int w, h, rot, x, y; public boolean hasXy; public int avgRgb; public byte[] gray; }
    public static final class Pt { public int dx, dy, rgb, tol; }
    public static final class Pts { public int baseRgb, baseTol, x, y; public boolean hasXy; public Pt[] pts; }

    // ---------------------------------------------------------------- 命名

    /** 与核心 vt_id_ok 同尺（v11）：可见字符 1..15（可用中文）、字节 ≤47、裸 "-" 拒收。 */
    public static boolean nameOk(String s) { return Regions.idOk(s); }

    // ---------------------------------------------------------------- 字节序小工具

    private static void w16(ByteArrayOutputStream o, int v) { o.write(v & 0xFF); o.write((v >> 8) & 0xFF); }
    private static void w32(ByteArrayOutputStream o, long v) {
        o.write((int) (v & 0xFF)); o.write((int) ((v >> 8) & 0xFF));
        o.write((int) ((v >> 16) & 0xFF)); o.write((int) ((v >> 24) & 0xFF));
    }
    private static int r16(byte[] b, int off) { return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8); }
    private static long r32(byte[] b, int off) {
        return (b[off] & 0xFFL) | ((b[off + 1] & 0xFFL) << 8)
                | ((b[off + 2] & 0xFFL) << 16) | ((b[off + 3] & 0xFFL) << 24);
    }

    // ---------------------------------------------------------------- .tmpl 编解码（纯函数）

    public static byte[] bytesTmpl(Tmpl t) {
        if (t == null || t.gray == null) return null;
        if (t.w < 1 || t.h < 1 || t.w > 4096 || t.h > 4096) return null;
        if (t.gray.length != t.w * t.h) return null;
        int avg = t.avgRgb < 0 ? 0 : (t.avgRgb & 0xFFFFFF);
        ByteArrayOutputStream o = new ByteArrayOutputStream(14 + t.gray.length + 8);
        o.write('V'); o.write('T'); o.write('M'); o.write('1');
        w32(o, 1);
        w16(o, t.w); w16(o, t.h);
        o.write(t.rot & 3);
        o.write(0);
        o.write(t.gray, 0, t.gray.length);
        w16(o, t.x < 0 ? 0 : t.x); w16(o, t.y < 0 ? 0 : t.y);
        o.write((avg >> 16) & 0xFF); o.write((avg >> 8) & 0xFF); o.write(avg & 0xFF);
        o.write(0);
        return o.toByteArray();
    }

    public static Tmpl parseTmpl(byte[] b) {
        if (b == null || b.length < 14) return null;
        if (b[0] != 'V' || b[1] != 'T' || b[2] != 'M' || b[3] != '1') return null;
        if (r32(b, 4) != 1) return null;
        int w = r16(b, 8), h = r16(b, 10);
        int rot = b[12] & 0xFF;
        if (w < 1 || h < 1 || w > 4096 || h > 4096 || rot > 3) return null;
        long need = 14L + (long) w * h;
        if (b.length < need) return null;
        Tmpl t = new Tmpl();
        t.w = w; t.h = h; t.rot = rot;
        t.gray = new byte[w * h];
        System.arraycopy(b, 14, t.gray, 0, w * h);
        t.avgRgb = -1; t.hasXy = false;
        int tail = (int) (b.length - need);
        if (tail >= 4) {
            t.x = r16(b, (int) need); t.y = r16(b, (int) need + 2);
            t.hasXy = true;
        }
        if (tail >= 8) {
            t.avgRgb = ((b[(int) need + 4] & 0xFF) << 16) | ((b[(int) need + 5] & 0xFF) << 8)
                    | (b[(int) need + 6] & 0xFF);
        }
        return t;
    }

    // ---------------------------------------------------------------- .pts 编解码（纯函数）

    public static byte[] bytesPts(Pts p) {
        if (p == null || p.pts == null) return null;
        int n = p.pts.length;
        if (n < 1 || n > 16) return null;
        if (p.baseTol < 0 || p.baseTol > 255) return null;
        for (Pt q : p.pts) {
            if (q == null || q.tol < 0 || q.tol > 255) return null;
            if (q.dx < -4096 || q.dx > 4096 || q.dy < -4096 || q.dy > 4096) return null;
        }
        ByteArrayOutputStream o = new ByteArrayOutputStream(18 + n * 10 + 4);
        o.write('V'); o.write('T'); o.write('P'); o.write('1');
        w32(o, 1);
        w16(o, n); w16(o, 0);
        w32(o, p.baseRgb & 0xFFFFFF);
        w16(o, p.baseTol);
        for (Pt q : p.pts) {
            w16(o, q.dx & 0xFFFF);
            w16(o, q.dy & 0xFFFF);
            w32(o, q.rgb & 0xFFFFFF);
            w16(o, q.tol);
        }
        w16(o, p.x < 0 ? 0 : p.x); w16(o, p.y < 0 ? 0 : p.y);
        return o.toByteArray();
    }

    public static Pts parsePts(byte[] b) {
        if (b == null || b.length < 18) return null;
        if (b[0] != 'V' || b[1] != 'T' || b[2] != 'P' || b[3] != '1') return null;
        if (r32(b, 4) != 1) return null;
        int n = r16(b, 8);
        long baseRgb = r32(b, 12);
        int baseTol = r16(b, 16);
        if (n < 1 || n > 16 || baseTol > 255) return null;
        long need = 18L + (long) n * 10;
        if (b.length < need) return null;
        Pts p = new Pts();
        p.baseRgb = (int) (baseRgb & 0xFFFFFF);
        p.baseTol = baseTol;
        p.pts = new Pt[n];
        for (int i = 0; i < n; i++) {
            int off = 18 + i * 10;
            Pt q = new Pt();
            q.dx = (short) r16(b, off);
            q.dy = (short) r16(b, off + 2);
            q.rgb = (int) (r32(b, off + 4) & 0xFFFFFF);
            q.tol = r16(b, off + 8);
            p.pts[i] = q;
        }
        p.hasXy = false;
        if (b.length >= need + 4) {
            p.x = r16(b, (int) need); p.y = r16(b, (int) need + 2);
            p.hasXy = true;
        }
        return p;
    }

    // ---------------------------------------------------------------- 文件层（su）

    private static String pathOf(String name, String suffix) { return dir + "/" + name + suffix; }

    public static List<String> namesTmpl() { return names(".tmpl", 5); }

    public static List<String> namesPts() { return names(".pts", 4); }

    private static List<String> names(String suffix, int cut) {
        List<String> out = new ArrayList<String>();
        Su.R r = Su.run("ls '" + dir + "' 2>/dev/null");
        if (!r.ok() || r.out == null) return out;
        for (String ln : r.out.split("\n")) {
            String fn = ln.trim();
            if (fn.endsWith(suffix)) out.add(fn.substring(0, fn.length() - cut));
        }
        Collections.sort(out);
        return out;
    }

    public static Tmpl readTmpl(String name) {
        if (!nameOk(name)) return null;
        byte[] b = Su.readBytes(pathOf(name, ".tmpl"));
        return b == null ? null : parseTmpl(b);
    }

    public static Pts readPts(String name) {
        if (!nameOk(name)) return null;
        byte[] b = Su.readBytes(pathOf(name, ".pts"));
        return b == null ? null : parsePts(b);
    }

    public static void writeTmpl(String name, Tmpl t) {
        if (!nameOk(name)) return;
        byte[] b = bytesTmpl(t);
        if (b == null) return;
        Su.run("mkdir -p '" + dir + "' && chmod 775 '" + dir + "'");
        Su.writeAtomic(pathOf(name, ".tmpl"), b);
    }

    public static void writePts(String name, Pts p) {
        if (!nameOk(name)) return;
        byte[] b = bytesPts(p);
        if (b == null) return;
        Su.run("mkdir -p '" + dir + "' && chmod 775 '" + dir + "'");
        Su.writeAtomic(pathOf(name, ".pts"), b);
    }

    public static boolean exists(String name) {
        if (!nameOk(name)) return false;
        return Su.fileExists(pathOf(name, ".tmpl")) || Su.fileExists(pathOf(name, ".pts"));
    }

    /** 删存在的那一个（.tmpl / .pts）；返回是否有删除。 */
    public static boolean delete(String name) {
        if (!nameOk(name)) return false;
        boolean any = false;
        String[] sufs = {".tmpl", ".pts"};
        for (String s : sufs) {
            String p = pathOf(name, s);
            if (Su.fileExists(p) && Su.run("rm -f '" + p + "'").ok()) any = true;
        }
        return any;
    }

    /** 改名 = 文件 mv（重名拒绝，不覆盖）。 */
    public static boolean rename(String oldName, String newName, boolean isPts) {
        if (!nameOk(oldName) || !nameOk(newName)) return false;
        String suffix = isPts ? ".pts" : ".tmpl";
        String from = pathOf(oldName, suffix), to = pathOf(newName, suffix);
        if (Su.fileExists(to)) return false;
        if (!Su.fileExists(from)) return false;
        return Su.run("mv '" + from + "' '" + to + "' && chmod 644 '" + to + "' && chown root:root '" + to + "'").ok();
    }

    // ---------------------------------------------------------------- 引用扫描

    /** 扫 live + 各方案引用（找图 ref / 找色多点 ref）；返回 "操作 <名> · <来源>"。 */
    public static List<String> refs(String name) {
        List<String> texts = new ArrayList<String>();
        List<String> labels = new ArrayList<String>();
        String live = Su.read(Ops.PATH);
        if (live != null) { texts.add(live); labels.add("当前"); }
        for (String s : Schemes.list()) {
            String t = Su.read(Schemes.DIR + "/" + s + "/ops.conf");
            if (t != null) { texts.add(t); labels.add("方案 " + s); }
        }
        return refsFromTexts(texts, labels, name);
    }

    static List<String> refsFromTexts(List<String> texts, List<String> labels, String name) {
        List<String> out = new ArrayList<String>();
        if (texts == null || name == null || name.isEmpty()) return out;
        for (int i = 0; i < texts.size(); i++) {
            try {
                Ops.File f = Ops.File.parse(texts.get(i));
                String label = (labels != null && i < labels.size()) ? labels.get(i) : "当前";
                for (Ops.Op op : f.ops) {
                    boolean hit = false;
                    for (Ops.Step st : op.steps) {
                        if (st.type == Ops.T_FINDIMAGE && name.equals(st.ref)) { hit = true; break; }
                        if (st.type == Ops.T_FINDCOLOR && st.a1 == 1 && name.equals(st.ref)) { hit = true; break; }
                    }
                    if (hit) out.add("操作 " + op.name + " · " + label);
                }
            } catch (Exception e) { /* 单源解析失败跳过 */ }
        }
        return out;
    }

    // ---------------------------------------------------------------- 灰度 / 缩略图

    /** 灰度公式（与核心/面板同一来源）：(77r+150g+29b+128)>>8。 */
    public static byte grayPx(int r, int g, int b) {
        return (byte) ((77 * r + 150 * g + 29 * b + 128) >> 8);
    }

    /** 帧 RGBA 缓冲区取区域灰度 + 平均色（颜色校验用；与面板写端同算式）。 */
    public static byte[] grayFromRgba(byte[] rgba, int w, int x, int y, int bw, int bh, int[] avgOut3) {
        if (rgba == null || w < 1 || bw < 1 || bh < 1) return null;
        if (x < 0 || y < 0 || x + bw > w) return null;
        byte[] g = new byte[bw * bh];
        long sr = 0, sg = 0, sb = 0;
        for (int yy = 0; yy < bh; yy++) {
            int rowBase = ((y + yy) * w + x) * 4;
            for (int xx = 0; xx < bw; xx++) {
                int p = rowBase + xx * 4;
                if (p + 2 >= rgba.length) return null;
                int r = rgba[p] & 0xFF, gg = rgba[p + 1] & 0xFF, b = rgba[p + 2] & 0xFF;
                g[yy * bw + xx] = grayPx(r, gg, b);
                sr += r; sg += gg; sb += b;
            }
        }
        if (avgOut3 != null && avgOut3.length >= 3) {
            long n = (long) bw * bh;
            avgOut3[0] = (int) (sr / n);
            avgOut3[1] = (int) (sg / n);
            avgOut3[2] = (int) (sb / n);
        }
        return g;
    }

    /** .tmpl 缩略图（灰度→RGB；scale 最近邻）。 */
    public static Bitmap thumbTmpl(String name, int maxPx) {
        Tmpl t = readTmpl(name);
        if (t == null || maxPx < 8) return null;
        int scale = Math.max(1, Math.max((t.w + maxPx - 1) / maxPx, (t.h + maxPx - 1) / maxPx));
        int tw = Math.max(1, t.w / scale), th = Math.max(1, t.h / scale);
        int[] px = new int[tw * th];
        for (int yy = 0; yy < th; yy++) {
            for (int xx = 0; xx < tw; xx++) {
                int gx = Math.min(t.w - 1, xx * scale), gy = Math.min(t.h - 1, yy * scale);
                int v = t.gray[gy * t.w + gx] & 0xFF;
                px[yy * tw + xx] = 0xFF000000 | (v << 16) | (v << 8) | v;
            }
        }
        return Bitmap.createBitmap(px, tw, th, Bitmap.Config.ARGB_8888);
    }

    /** .pts 缩略图（基准色块 + 参考点示意）。 */
    public static Bitmap thumbPts(String name, int maxPx) {
        Pts p = readPts(name);
        if (p == null || maxPx < 8) return null;
        int[] px = new int[maxPx * maxPx];
        int bg = 0xFF1D2126;
        for (int i = 0; i < px.length; i++) px[i] = bg;
        int cx = maxPx / 2, cy = maxPx / 2;
        int base = 0xFF000000 | (p.baseRgb & 0xFFFFFF);
        for (int yy = cy - 8; yy <= cy + 8; yy++)
            for (int xx = cx - 8; xx <= cx + 8; xx++)
                if (xx >= 0 && yy >= 0 && xx < maxPx && yy < maxPx) px[yy * maxPx + xx] = base;
        int maxOff = 1;
        for (Pt q : p.pts) maxOff = Math.max(maxOff, Math.max(Math.abs(q.dx), Math.abs(q.dy)));
        double s = (maxPx / 2.0 - 12) / maxOff;
        if (s > 4) s = 4;
        for (Pt q : p.pts) {
            int mx = cx + (int) Math.round(q.dx * s);
            int my = cy + (int) Math.round(q.dy * s);
            for (int dy2 = 0; dy2 < 2; dy2++)
                for (int dx2 = 0; dx2 < 2; dx2++) {
                    int xx = mx + dx2, yy = my + dy2;
                    if (xx >= 0 && yy >= 0 && xx < maxPx && yy < maxPx) px[yy * maxPx + xx] = 0xFFE9EDF1;
                }
        }
        return Bitmap.createBitmap(px, maxPx, maxPx, Bitmap.Config.ARGB_8888);
    }
}
