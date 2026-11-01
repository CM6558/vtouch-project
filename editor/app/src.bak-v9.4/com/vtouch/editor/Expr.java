package com.vtouch.editor;

import java.util.List;

/**
 * 表达式校验（仓库 src/vt_expr.c 的 vt_expr_check 逐字移植，静态模式）：
 * 语法 + - * /、括号、一元负号；函数 atan2/sin/cos/abs/min/max/sqrt（三角函数按度、atan2(0,0)=0）；
 * 操作数 = 数字 / 预置名（tdx/tdy/tux/tuy/tms/fx/fy）/ 自定义变量名（表随调用传入）/ v4 旧名 r1..r4（兼容）；
 * 空白忽略、全小写精确匹配；长度 ≤63、括号嵌套 ≤8、token ≤128（token 上限先于长度检查）；
 * 静态常量折叠 —— 「除零 / 负数开方 / 结果非有限」只在完全由常量决定时拦下。
 * check 通过返回 null；否则返回短中文原因（与核心逐字同词）。
 */
public final class Expr {
    private static final int LEN_MAX = 63, DEPTH_MAX = 8, TOK_MAX = 128;
    private static final double PI = 3.14159265358979323846;

    private static final int TOK_END = 0, TOK_NUM = 1, TOK_NAME = 2, TOK_PLUS = 3, TOK_MINUS = 4,
            TOK_STAR = 5, TOK_SLASH = 6, TOK_LP = 7, TOK_RP = 8, TOK_COMMA = 9;
    private static final int K_UNKNOWN = 0, K_VAR = 1, K_RES = 2, K_CUSTOM = 3, K_FUNC = 4;
    private static final int F_ATAN2 = 0, F_SIN = 1, F_COS = 2, F_ABS = 3, F_MIN = 4, F_MAX = 5, F_SQRT = 6;

    private final String s;
    private final List<String> names;
    private int p;
    private int tok, depth, tcount, nameS, nameL, idx;
    private double num, v;
    private boolean known;
    private String why;

    private Expr(String s, List<String> names) {
        this.s = s == null ? "" : s;
        this.names = names;
    }

    /** 校验表达式：通过返回 null；否则返回短中文原因。 */
    public static String check(String text, List<String> names) {
        if (text == null || text.isEmpty()) return "表达式为空";
        Expr e = new Expr(text, names);
        e.run();
        return e.why;
    }

    private void fail(String msg) {
        if (why == null) why = msg;
    }

    private void tok() {
        num = 0;
        nameS = 0;
        nameL = 0;
        while (p < s.length()) {
            char ch = s.charAt(p);
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') p++;
            else break;
        }
        if (p >= s.length()) { tok = TOK_END; return; }
        char ch = s.charAt(p);
        if (ch >= '0' && ch <= '9') {
            double d = 0.0;
            while (p < s.length() && s.charAt(p) >= '0' && s.charAt(p) <= '9') {
                d = d * 10.0 + (s.charAt(p) - '0');
                p++;
            }
            if (p < s.length() && s.charAt(p) == '.') {
                p++;
                if (p >= s.length() || !(s.charAt(p) >= '0' && s.charAt(p) <= '9')) {
                    tok = TOK_END; fail("非法数字"); return;
                }
                double scale = 0.1;
                while (p < s.length() && s.charAt(p) >= '0' && s.charAt(p) <= '9') {
                    d += (s.charAt(p) - '0') * scale;
                    scale *= 0.1;
                    p++;
                }
            }
            num = d;
            tok = TOK_NUM;
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
            int st = p;
            while (p < s.length()) {
                char q = s.charAt(p);
                if ((q >= 'a' && q <= 'z') || (q >= 'A' && q <= 'Z')
                        || (q >= '0' && q <= '9') || q == '_') p++;
                else break;
            }
            nameS = st;
            nameL = p - st;
            tok = TOK_NAME;
        } else if (ch == '+') { p++; tok = TOK_PLUS; }
        else if (ch == '-') { p++; tok = TOK_MINUS; }
        else if (ch == '*') { p++; tok = TOK_STAR; }
        else if (ch == '/') { p++; tok = TOK_SLASH; }
        else if (ch == '(') { p++; tok = TOK_LP; }
        else if (ch == ')') { p++; tok = TOK_RP; }
        else if (ch == ',') { p++; tok = TOK_COMMA; }
        else if (ch == '.') { p++; tok = TOK_END; fail("非法数字"); return; }
        else { p++; tok = TOK_END; fail("非法字符"); return; }
        if (++tcount > TOK_MAX) { fail("表达式过长"); tok = TOK_END; }
    }

    private String curName() { return s.substring(nameS, nameS + nameL); }

    /** 名字分类：预置 → 函数 → 自定义表（精确匹配）→ v4 旧名 r1..r4 → 未知。 */
    private int nameKind(String nm) {
        if (nm.equals("tdx")) { idx = 0; return K_VAR; }
        if (nm.equals("tdy")) { idx = 1; return K_VAR; }
        if (nm.equals("tux")) { idx = 2; return K_VAR; }
        if (nm.equals("tuy")) { idx = 3; return K_VAR; }
        if (nm.equals("tms")) { idx = 4; return K_VAR; }
        if (nm.equals("fx")) { idx = 0; return K_RES; }
        if (nm.equals("fy")) { idx = 1; return K_RES; }
        if (nm.equals("atan2")) { idx = F_ATAN2; return K_FUNC; }
        if (nm.equals("sin")) { idx = F_SIN; return K_FUNC; }
        if (nm.equals("cos")) { idx = F_COS; return K_FUNC; }
        if (nm.equals("abs")) { idx = F_ABS; return K_FUNC; }
        if (nm.equals("min")) { idx = F_MIN; return K_FUNC; }
        if (nm.equals("max")) { idx = F_MAX; return K_FUNC; }
        if (nm.equals("sqrt")) { idx = F_SQRT; return K_FUNC; }
        if (names != null) {
            int lim = Math.min(names.size(), 16);
            for (int i = 0; i < lim; i++) {
                String n = names.get(i);
                if (n != null && n.length() >= 1 && n.length() < 16 && n.equals(nm)) { idx = i; return K_CUSTOM; }
            }
        }
        if (nm.length() == 2 && nm.charAt(0) == 'r' && nm.charAt(1) >= '1' && nm.charAt(1) <= '4') {
            idx = nm.charAt(1) - '1';
            return K_RES;
        }
        return K_UNKNOWN;
    }

    private boolean open() {
        depth++;
        if (depth > DEPTH_MAX) { fail("嵌套过深"); return false; }
        tok();
        return why == null;
    }

    private boolean close() {
        if (why != null) return false;
        if (tok != TOK_RP) { fail("括号不配对"); return false; }
        depth--;
        tok();
        return why == null;
    }

    private void call(int f) {
        double[] av = new double[2];
        boolean[] ak = new boolean[2];
        int nargs = 0;
        tok();
        if (why != null) return;
        if (tok != TOK_LP) { fail("括号不配对"); return; }
        if (!open()) return;
        if (tok != TOK_RP) {
            for (;;) {
                expr();
                if (why != null) return;
                if (nargs < 2) { av[nargs] = v; ak[nargs] = known; }
                nargs++;
                if (tok != TOK_COMMA) break;
                tok();
                if (why != null) return;
            }
        }
        if (!close()) return;
        boolean needTwo = (f == F_ATAN2 || f == F_MIN || f == F_MAX);
        if (needTwo ? (nargs != 2) : (nargs != 1)) { fail("参数个数不对"); return; }
        switch (f) {
            case F_ATAN2:
                known = ak[0] && ak[1];
                if (known) {
                    if (av[0] == 0.0 && av[1] == 0.0) v = 0.0;
                    else {
                        v = Math.atan2(av[0], av[1]) / PI * 180.0;
                        if (v == -180.0) v = 180.0;
                    }
                }
                break;
            case F_SIN:
                known = ak[0];
                if (known) v = Math.sin(av[0] * (PI / 180.0));
                break;
            case F_COS:
                known = ak[0];
                if (known) v = Math.cos(av[0] * (PI / 180.0));
                break;
            case F_ABS:
                known = ak[0];
                if (known) v = Math.abs(av[0]);
                break;
            case F_MIN:
                known = ak[0] && ak[1];
                if (known) v = av[0] < av[1] ? av[0] : av[1];
                break;
            case F_MAX:
                known = ak[0] && ak[1];
                if (known) v = av[0] > av[1] ? av[0] : av[1];
                break;
            default: // F_SQRT
                if (ak[0] && av[0] < 0.0) { fail("负数开方"); return; }
                known = ak[0];
                if (known) v = Math.sqrt(av[0]);
                break;
        }
        if (known && !isFinite(v)) fail("结果非有限");
    }

    private void primary() {
        if (why != null) return;
        switch (tok) {
            case TOK_NUM:
                v = num;
                known = true;
                tok();
                return;
            case TOK_NAME: {
                String nm = curName();
                int kind = nameKind(nm);
                if (kind == K_VAR || kind == K_RES || kind == K_CUSTOM) {
                    known = false;   // 静态模式：变量一律未知值
                    tok();
                    return;
                }
                if (kind == K_FUNC) { call(idx); return; }
                fail("未知名字");
                return;
            }
            case TOK_LP:
                if (!open()) return;
                expr();
                if (why != null) return;
                close();
                return;
            default:
                fail("缺少操作数");
        }
    }

    private void unary() {
        if (why != null) return;
        if (tok == TOK_MINUS) {
            tok();
            if (why != null) return;
            unary();
            if (why != null) return;
            if (known) v = -v;
            return;
        }
        primary();
    }

    private void term() {
        if (why != null) return;
        unary();
        while (why == null && (tok == TOK_STAR || tok == TOK_SLASH)) {
            int op = tok;
            boolean lk = known;
            double lv = v;
            tok();
            if (why != null) return;
            unary();
            if (why != null) return;
            if (op == TOK_SLASH) {
                if (known && v == 0.0) { fail("除零"); return; }
                if (lk && known) {
                    v = lv / v;
                    if (!isFinite(v)) { fail("结果非有限"); return; }
                } else known = false;
            } else {
                if (lk && known) {
                    v = lv * v;
                    if (!isFinite(v)) { fail("结果非有限"); return; }
                } else known = false;
            }
        }
    }

    private void expr() {
        if (why != null) return;
        term();
        while (why == null && (tok == TOK_PLUS || tok == TOK_MINUS)) {
            int op = tok;
            boolean lk = known;
            double lv = v;
            tok();
            if (why != null) return;
            term();
            if (why != null) return;
            if (lk && known) {
                v = (op == TOK_PLUS) ? lv + v : lv - v;
                if (!isFinite(v)) { fail("结果非有限"); return; }
            } else known = false;
        }
    }

    private static boolean isFinite(double d) {
        return !Double.isNaN(d) && !Double.isInfinite(d);
    }

    private void run() {
        for (;;) {                       // 1) 词法预扫（token 上限先于长度检查）
            tok();
            if (why != null) return;
            if (tok == TOK_END) break;
        }
        if (tcount == 0) { fail("表达式为空"); return; }
        if (s.length() > LEN_MAX) { fail("超过 63 字符"); return; }
        p = 0;                           // 2) 解析（游标回串首）
        tcount = 0;
        depth = 0;
        tok();
        if (why == null) expr();
        if (why == null && tok != TOK_END) fail(tok == TOK_RP ? "括号不配对" : "多余字符");
    }
}
