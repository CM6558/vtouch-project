package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/** vt-script 词法器（T1.1）。规范：docs/VT_SCRIPT_V1_SPEC.md §2（修订 r1）；错误词 §6。
 *  纯 Java、零 Android 依赖（离机核 ~/vtchk-host/VtScriptCheck）。 */
public final class VtLexer {

    /** 词法结果：token 流（含 EOF）+ 首个错误（`<词>` 或 `<词>（近因）` + 行列）。 */
    public static final class R {
        public List<VtTok> toks = new ArrayList<VtTok>();
        public String err;                // null = 通过
        public int line = 1, col = 1;     // 出错位置（err != null 时有效）
    }

    private final String s;
    private final int n;
    private int p, line = 1, col = 1;
    private final List<VtTok> toks = new ArrayList<VtTok>();
    private String err;
    private int errLine = 1, errCol = 1;

    private VtLexer(String src) { s = src; n = src.length(); }

    /** 全量词法：首错即停（已扫到的 token 保留；调用方以 err 为准）。 */
    public static R tokenize(String src) {
        VtLexer lx = new VtLexer(src == null ? "" : src);
        lx.run();
        R r = new R();
        r.toks = lx.toks;
        r.err = lx.err;
        r.line = lx.errLine;
        r.col = lx.errCol;
        return r;
    }

    private void run() {
        while (p < n && err == null) {
            char c = s.charAt(p);
            if (c == ' ' || c == '\t' || c == '\r') { p++; col++; }
            else if (c == '\n') { sep("\n"); p++; line++; col = 1; }
            else if (c == '#') { while (p < n && s.charAt(p) != '\n') { p++; col++; } }
            else if (c == ';') { sep(";"); p++; col++; }
            else if (c >= '0' && c <= '9') num();
            else if (c == '"') str();
            else if (idStart(c)) ident();
            else op();
        }
        if (err == null) {
            VtTok t = new VtTok();
            t.kind = VtTok.EOF; t.line = line; t.col = col;
            toks.add(t);
        }
    }

    private void fail(String word, String why, int l, int c) {
        if (err != null) return;
        err = (why == null) ? word : word + "（" + why + "）";
        errLine = l; errCol = c;
    }

    private void addTok(int kind, String text, int l, int c) {
        VtTok t = new VtTok();
        t.kind = kind; t.text = text; t.line = l; t.col = c;
        toks.add(t);
    }

    /** 语句分隔（换行或 ;）：抑制前导、折叠连续。 */
    private void sep(String txt) {
        if (toks.isEmpty()) return;
        if (toks.get(toks.size() - 1).kind == VtTok.SEP) return;
        addTok(VtTok.SEP, txt, line, col);
    }

    private void num() {
        int tl = line, tc = col, st = p;
        if (s.charAt(p) == '0' && p + 1 < n && (s.charAt(p + 1) == 'x' || s.charAt(p + 1) == 'X')) {
            p += 2; col += 2;
            int hd = 0;
            long v = 0;
            while (p < n && isHex(s.charAt(p))) { v = v * 16 + hexv(s.charAt(p)); p++; col++; hd++; }
            if (hd < 1 || hd > 8) { fail("非法数字", "0x 需 1..8 位十六进制", tl, tc); return; }
            VtTok t = new VtTok();
            t.kind = VtTok.NUM; t.text = s.substring(st, p); t.num = (double) v; t.line = tl; t.col = tc;
            toks.add(t);
            return;
        }
        while (p < n && s.charAt(p) >= '0' && s.charAt(p) <= '9') { p++; col++; }
        if (p < n && s.charAt(p) == '.') {
            if (p + 1 >= n || !(s.charAt(p + 1) >= '0' && s.charAt(p + 1) <= '9')) {
                fail("非法数字", "小数点后须有数字", tl, tc);
                return;
            }
            p++; col++;
            while (p < n && s.charAt(p) >= '0' && s.charAt(p) <= '9') { p++; col++; }
        }
        VtTok t = new VtTok();
        t.kind = VtTok.NUM; t.text = s.substring(st, p); t.num = Double.parseDouble(t.text);
        t.line = tl; t.col = tc;
        toks.add(t);
    }

    private void ident() {
        int tl = line, tc = col, st = p;
        while (p < n && idPart(s.charAt(p))) { p++; col++; }
        String w = s.substring(st, p);
        VtTok t = new VtTok();
        t.line = tl; t.col = tc; t.text = w;
        int k = VtTok.kwOf(w);
        if (k > 0) { t.kind = VtTok.KW; t.kw = k; }
        else t.kind = VtTok.IDENT;
        toks.add(t);
    }

    private void op() {
        char c = s.charAt(p);
        char d = (p + 1 < n) ? s.charAt(p + 1) : '\0';
        String two = null;
        if (c == '=' && d == '=') two = "==";
        else if (c == '!' && d == '=') two = "!=";
        else if (c == '<' && d == '=') two = "<=";
        else if (c == '>' && d == '=') two = ">=";
        else if (c == '&' && d == '&') two = "&&";
        else if (c == '|' && d == '|') two = "||";
        if (two != null) { addTok(VtTok.OP, two, line, col); p += 2; col += 2; return; }
        if ("+-*/%()[]{},!=<>".indexOf(c) >= 0) { addTok(VtTok.OP, String.valueOf(c), line, col); p++; col++; return; }
        fail("非法字符", "'" + c + "'", line, col);
        p++; col++;
    }

    /** 字符串：转义 \" \\ \n \t；{{ }} 字面花括号；{名}/{表[下标]} 插值；单行（换行 = 未闭合）。 */
    private void str() {
        int tl = line, tc = col;
        p++; col++;                                     // 开引号
        int rawStart = p;
        StringBuilder lit = new StringBuilder();
        List<Object> segs = new ArrayList<Object>();
        boolean closed = false;
        while (p < n) {
            char c = s.charAt(p);
            if (c == '"') { p++; col++; closed = true; break; }
            if (c == '\n') { fail("字符串未闭合", null, tl, tc); return; }
            if (c == '\\') {
                if (p + 1 >= n) { fail("字符串未闭合", null, tl, tc); return; }
                char d = s.charAt(p + 1);
                if (d == '"') lit.append('"');
                else if (d == '\\') lit.append('\\');
                else if (d == 'n') lit.append('\n');
                else if (d == 't') lit.append('\t');
                else { fail("字符串转义非法", "\\" + d, line, col); return; }
                p += 2; col += 2;
                continue;
            }
            if (c == '{') {
                if (p + 1 < n && s.charAt(p + 1) == '{') { lit.append('{'); p += 2; col += 2; continue; }
                if (lit.length() > 0) { segs.add(lit.toString()); lit.setLength(0); }
                int il = line, ic = col;
                p++; col++;
                StringBuilder sn = new StringBuilder();
                boolean bad = false;
                while (p < n && s.charAt(p) != '}') {
                    char x = s.charAt(p);
                    if (x == '\n' || x == '{') { bad = true; break; }
                    sn.append(x); p++; col++;
                }
                if (bad || p >= n || s.charAt(p) != '}') { fail("插值非法", "未闭合或含嵌套 {", il, ic); return; }
                p++; col++;                              // 吃 '}'
                VtTok.Interp ip = parseInterp(sn.toString(), il, ic);
                if (ip == null) { fail("插值非法", "{" + sn.toString().trim() + "}", il, ic); return; }
                segs.add(ip);
                continue;
            }
            if (c == '}') {
                if (p + 1 < n && s.charAt(p + 1) == '}') { lit.append('}'); p += 2; col += 2; continue; }
                fail("插值非法", "孤立 }", line, col);
                return;
            }
            lit.append(c); p++; col++;
        }
        if (!closed) { fail("字符串未闭合", null, tl, tc); return; }
        if (lit.length() > 0 || segs.isEmpty()) segs.add(lit.toString());
        VtTok t = new VtTok();
        t.kind = VtTok.STR; t.raw = s.substring(rawStart, p - 1); t.segs = segs;
        t.line = tl; t.col = tc;
        toks.add(t);
    }

    /** 插值内容：`名` 或 `名[下标]`（下标 = 全数字或合法名；两侧允许空白）。非法 = null。 */
    private static VtTok.Interp parseInterp(String raw, int l, int c) {
        String t = raw.trim();
        if (t.isEmpty()) return null;
        int b = t.indexOf('[');
        String nm = (b >= 0) ? t.substring(0, b).trim() : t;
        if (!nameOk(nm)) return null;
        VtTok.Interp ip = new VtTok.Interp();
        ip.name = nm; ip.line = l; ip.col = c;
        if (b >= 0) {
            if (t.charAt(t.length() - 1) != ']') return null;
            String sub = t.substring(b + 1, t.length() - 1).trim();
            if (sub.isEmpty()) return null;
            ip.hasIdx = true;
            if (allDigits(sub)) { ip.idxNum = true; ip.idx = Double.parseDouble(sub); }
            else if (nameOk(sub)) { ip.idxName = sub; }
            else return null;
        } else if (t.indexOf(']') >= 0) return null;
        return ip;
    }

    /** 名字形体（词法级）：字母/_/非 ASCII 起头；字母/数字/_/非 ASCII 续；不含 `-`（修订 r1）。 */
    private static boolean nameOk(String x) {
        if (x.isEmpty()) return false;
        for (int i = 0; i < x.length(); i++) {
            char c = x.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c > 0x7F) continue;
            if (i > 0 && c >= '0' && c <= '9') continue;
            return false;
        }
        return true;
    }

    private static boolean allDigits(String x) {
        if (x.isEmpty()) return false;
        for (int i = 0; i < x.length(); i++) { char c = x.charAt(i); if (c < '0' || c > '9') return false; }
        return true;
    }

    private static boolean idStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c > 0x7F;
    }
    private static boolean idPart(char c) {
        return idStart(c) || (c >= '0' && c <= '9');
    }
    private static boolean isHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    private static int hexv(char c) {
        return (c <= '9') ? (c - '0') : ((c >= 'a') ? (c - 'a' + 10) : (c - 'A' + 10));
    }
}
