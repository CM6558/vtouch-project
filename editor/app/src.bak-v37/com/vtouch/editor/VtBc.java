package com.vtouch.editor;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

/** VTBC v1 容器（T2.1）：写 / 读 / 结构自检。格式见 docs/VTBC_V1.md。
 *  纯 Java、零 Android 依赖（离机核 VtScriptCheck → bc 区段）。 */
public final class VtBc {

    public static final int VERSION = 1;
    public static final int MAX_BLOB = 32768;

    /* ---- 结构 ---- */
    public static final class Const { public int kind; public double num; public String str; }   // kind 0=num 1=str
    public static final class Fn { public int entry; public int locals; public int nargs; }
    public static final class Line { public int off; public int line; }
    public static final class Name { public int kind; public int index; public int nameConst; }   // kind 0变量 1表 2函数

    public static final class Blob {
        public int flags;
        public int srcHash;
        public byte[] code = new byte[0];
        public List<Const> consts = new ArrayList<Const>();
        public int slotCount = 7;
        public int mainLocals;
        public List<Integer> tables = new ArrayList<Integer>();
        public List<Fn> fns = new ArrayList<Fn>();
        public List<Line> lines = new ArrayList<Line>();
        public List<Name> names = new ArrayList<Name>();
    }

    /** 源码 UTF-8 的 FNV-1a 32 位（源↔字节码对账）。 */
    public static int hash(String src) {
        byte[] b;
        try { b = src.getBytes("UTF-8"); } catch (Exception e) { b = src.getBytes(); }
        int h = 0x811C9DC5;
        for (int i = 0; i < b.length; i++) { h ^= (b[i] & 0xFF); h *= 0x01000193; }
        return h;
    }

    /* ---- 小端写 ---- */
    private static void w16(ByteArrayOutputStream o, int v) { o.write(v & 0xFF); o.write((v >>> 8) & 0xFF); }
    private static void w32(ByteArrayOutputStream o, int v) {
        o.write(v & 0xFF); o.write((v >>> 8) & 0xFF); o.write((v >>> 16) & 0xFF); o.write((v >>> 24) & 0xFF);
    }
    private static void w64(ByteArrayOutputStream o, double d) {
        long v = Double.doubleToLongBits(d);
        for (int i = 0; i < 8; i++) o.write((int) ((v >>> (8 * i)) & 0xFF));
    }
    private static void wStr(ByteArrayOutputStream o, String s) {
        byte[] b;
        try { b = (s == null ? "" : s).getBytes("UTF-8"); } catch (Exception e) { b = (s == null ? "" : s).getBytes(); }
        w32(o, b.length);
        o.write(b, 0, b.length);
    }

    /** 写：写端自检不过 → IllegalArgumentException（`程序过大` 口径在内）。 */
    public static byte[] write(Blob b) {
        for (int i = 0; i < b.names.size(); i++) {
            Name n = b.names.get(i);
            if (n.kind > 2) throw new IllegalArgumentException("名字表 kind 非法");
            if (n.nameConst < 0 || n.nameConst >= b.consts.size()) throw new IllegalArgumentException("名字表常量越界");
            if (b.consts.get(n.nameConst).kind != 1) throw new IllegalArgumentException("名字必须指向字符串常量");
        }
        for (int i = 0; i < b.tables.size(); i++) {
            int t = b.tables.get(i).intValue();
            if (t < 1 || t > 4096) throw new IllegalArgumentException("表长度非法");
        }
        for (int i = 0; i < b.fns.size(); i++) {
            Fn f = b.fns.get(i);
            if (f.entry > b.code.length) throw new IllegalArgumentException("函数入口越界");
            if (f.locals < f.nargs) throw new IllegalArgumentException("局部槽 < 形参");
        }

        ByteArrayOutputStream rest = new ByteArrayOutputStream(256);
        w32(rest, b.code.length);
        rest.write(b.code, 0, b.code.length);
        w32(rest, b.consts.size());
        for (int i = 0; i < b.consts.size(); i++) {
            Const c = b.consts.get(i);
            rest.write(c.kind & 0xFF);
            if (c.kind == 0) w64(rest, c.num);
            else if (c.kind == 1) wStr(rest, c.str);
            else throw new IllegalArgumentException("常量 kind 非法");
        }
        w32(rest, b.slotCount);
        w32(rest, b.mainLocals);
        w32(rest, b.tables.size());
        for (int i = 0; i < b.tables.size(); i++) w32(rest, b.tables.get(i).intValue());
        w32(rest, b.fns.size());
        for (int i = 0; i < b.fns.size(); i++) {
            Fn f = b.fns.get(i);
            w32(rest, f.entry); w32(rest, f.locals); rest.write(f.nargs & 0xFF);
        }
        w32(rest, b.lines.size());
        for (int i = 0; i < b.lines.size(); i++) {
            Line l = b.lines.get(i);
            w32(rest, l.off); w32(rest, l.line);
        }
        w32(rest, b.names.size());
        for (int i = 0; i < b.names.size(); i++) {
            Name n = b.names.get(i);
            rest.write(n.kind & 0xFF); w32(rest, n.index); w32(rest, n.nameConst);
        }

        int total = 16 + rest.size();
        if (total > MAX_BLOB) throw new IllegalArgumentException("程序过大（字节码 >32KB）");

        ByteArrayOutputStream o = new ByteArrayOutputStream(total);
        o.write('V'); o.write('T'); o.write('B'); o.write('C');
        w16(o, VERSION);
        w16(o, b.flags);
        w32(o, b.srcHash);
        w32(o, total);
        byte[] rb = rest.toByteArray();
        o.write(rb, 0, rb.length);
        return o.toByteArray();
    }

    /* ---- 读 ---- */
    public static final class R { public Blob blob; public String err; }

    public static R read(byte[] d) {
        R r = new R();
        try {
            r.blob = rd(d);
        } catch (IllegalArgumentException e) {
            r.err = e.getMessage();
        }
        return r;
    }

    private static final class Rd {
        final byte[] d; int p;
        Rd(byte[] x) { d = x; }
        void need(int n) { if (p + n > d.length) fail("截断"); }
        int u8() { need(1); return d[p++] & 0xFF; }
        int u16() { need(2); int v = (d[p] & 0xFF) | ((d[p + 1] & 0xFF) << 8); p += 2; return v; }
        int u32() { need(4); int v = (d[p] & 0xFF) | ((d[p + 1] & 0xFF) << 8) | ((d[p + 2] & 0xFF) << 16) | ((d[p + 3] & 0xFF) << 24); p += 4; return v; }
        double f64() { need(8); long v = 0; for (int i = 0; i < 8; i++) v |= ((long) (d[p + i] & 0xFF)) << (8 * i); p += 8; return Double.longBitsToDouble(v); }
        String str() {
            int n = u32();
            if (n < 0 || p + n > d.length) fail("字符串越界");
            try { String s = new String(d, p, n, "UTF-8"); p += n; return s; }
            catch (Exception e) { fail("UTF-8 非法"); return null; }
        }
        void fail(String m) { throw new IllegalArgumentException("VTBC: " + m); }
    }

    private static Blob rd(byte[] d) {
        if (d.length < 16) throw new IllegalArgumentException("VTBC: 过短");
        if (d[0] != 'V' || d[1] != 'T' || d[2] != 'B' || d[3] != 'C') throw new IllegalArgumentException("VTBC: 魔数不符");
        Rd r = new Rd(d); r.p = 4;
        if (r.u16() != VERSION) throw new IllegalArgumentException("VTBC: 版本不符");
        Blob b = new Blob();
        b.flags = r.u16();
        if (b.flags != 0) throw new IllegalArgumentException("VTBC: flags 非 0");
        b.srcHash = r.u32();
        if (r.u32() != d.length) throw new IllegalArgumentException("VTBC: 长度不符");
        int cl = r.u32();
        if (cl < 0 || r.p + cl > d.length) throw new IllegalArgumentException("VTBC: 代码区越界");
        b.code = new byte[cl];
        System.arraycopy(d, r.p, b.code, 0, cl);
        r.p += cl;
        int cn = r.u32();
        if (cn < 0 || cn > d.length) throw new IllegalArgumentException("VTBC: 常量数越界");
        for (int i = 0; i < cn; i++) {
            Const c = new Const();
            c.kind = r.u8();
            if (c.kind == 0) c.num = r.f64();
            else if (c.kind == 1) c.str = r.str();
            else throw new IllegalArgumentException("VTBC: 常量 kind 非法");
            b.consts.add(c);
        }
        b.slotCount = r.u32();
        if (b.slotCount < 7) throw new IllegalArgumentException("VTBC: 槽数 <7");
        b.mainLocals = r.u32();
        int tn = r.u32();
        if (tn < 0 || tn > 65536) throw new IllegalArgumentException("VTBC: 表数越界");
        for (int i = 0; i < tn; i++) {
            int len = r.u32();
            if (len < 1 || len > 4096) throw new IllegalArgumentException("VTBC: 表长度非法");
            b.tables.add(Integer.valueOf(len));
        }
        int fn = r.u32();
        if (fn < 0 || fn > 65536) throw new IllegalArgumentException("VTBC: 函数数越界");
        for (int i = 0; i < fn; i++) {
            Fn f = new Fn();
            f.entry = r.u32();
            f.locals = r.u32();
            f.nargs = r.u8();
            if (f.entry > cl) throw new IllegalArgumentException("VTBC: 函数入口越界");
            if (f.locals < f.nargs) throw new IllegalArgumentException("VTBC: 局部槽 < 形参");
            b.fns.add(f);
        }
        int ln = r.u32();
        if (ln < 0 || ln > 1048576) throw new IllegalArgumentException("VTBC: 行号数越界");
        for (int i = 0; i < ln; i++) {
            Line l = new Line();
            l.off = r.u32();
            l.line = r.u32();
            if (l.off > cl) throw new IllegalArgumentException("VTBC: 行号偏移越界");
            b.lines.add(l);
        }
        int nn = r.u32();
        if (nn < 0 || nn > 65536) throw new IllegalArgumentException("VTBC: 名字数越界");
        for (int i = 0; i < nn; i++) {
            Name n = new Name();
            n.kind = r.u8();
            if (n.kind > 2) throw new IllegalArgumentException("VTBC: 名字 kind 非法");
            n.index = r.u32();
            n.nameConst = r.u32();
            if (n.nameConst < 0 || n.nameConst >= b.consts.size() || b.consts.get(n.nameConst).kind != 1)
                throw new IllegalArgumentException("VTBC: 名字常量非法");
            b.names.add(n);
        }
        if (r.p != d.length) throw new IllegalArgumentException("VTBC: 尾部多余字节");
        return b;
    }

    /** 结构自检：写 → 读 → 逐字段比对。返回 null = 过。 */
    public static String selfCheck(Blob b) {
        byte[] x;
        try { x = write(b); } catch (IllegalArgumentException e) { return "写拒: " + e.getMessage(); }
        R r = read(x);
        if (r.err != null) return "读拒: " + r.err;
        Blob c = r.blob;
        if (c.flags != b.flags || c.srcHash != b.srcHash || c.slotCount != b.slotCount) return "头字段不符";
        if (c.mainLocals != b.mainLocals) return "mainLocals 不符";
        if (c.code.length != b.code.length) return "code 长度不符";
        for (int i = 0; i < c.code.length; i++) if (c.code[i] != b.code[i]) return "code 字节不符";
        if (c.consts.size() != b.consts.size()) return "常量数不符";
        for (int i = 0; i < c.consts.size(); i++) {
            Const a = c.consts.get(i), z = b.consts.get(i);
            if (a.kind != z.kind) return "常量 kind 不符";
            if (a.kind == 0) {
                if (Double.doubleToLongBits(a.num) != Double.doubleToLongBits(z.num)) return "常量值不符";
            } else if (!a.str.equals(z.str)) return "常量串不符";
        }
        if (!c.tables.equals(b.tables)) return "表不符";
        if (c.fns.size() != b.fns.size()) return "函数数不符";
        for (int i = 0; i < c.fns.size(); i++) {
            Fn a = c.fns.get(i), z = b.fns.get(i);
            if (a.entry != z.entry || a.locals != z.locals || a.nargs != z.nargs) return "函数不符";
        }
        if (c.lines.size() != b.lines.size()) return "行号数不符";
        for (int i = 0; i < c.lines.size(); i++) {
            Line a = c.lines.get(i), z = b.lines.get(i);
            if (a.off != z.off || a.line != z.line) return "行号不符";
        }
        if (c.names.size() != b.names.size()) return "名字数不符";
        for (int i = 0; i < c.names.size(); i++) {
            Name a = c.names.get(i), z = b.names.get(i);
            if (a.kind != z.kind || a.index != z.index || a.nameConst != z.nameConst) return "名字不符";
        }
        return null;
    }
}
