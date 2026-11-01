package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/** vt-script 语法器（T1.2）：token 流 → AST（VtAst）。规范 §7。
 *  首错即停；错误词对齐 §6（语法错误 / 表达式为空 / 块未闭合 / 否则不匹配 / 括号不匹配 / 保留字 / 表长度非法）。
 *  纯 Java、零 Android 依赖（离机核 ~/vtchk-host/VtScriptCheck）。 */
public final class VtParser {

    /** 语法结果：file（err == null 时有效）+ 首个错误。 */
    public static final class R {
        public VtAst.Block file;
        public String err;
        public int line = 1, col = 1;
    }

    private static final class Fail extends RuntimeException {
        final String msg; final int line, col;
        Fail(String m, int l, int c) { super(m); msg = m; line = l; col = c; }
    }

    private final List<VtTok> t;
    private int pos;

    private VtParser(List<VtTok> toks) { t = toks; }

    public static R parse(String src) {
        VtLexer.R lr = VtLexer.tokenize(src);
        R r = new R();
        if (lr.err != null) { r.err = lr.err; r.line = lr.line; r.col = lr.col; return r; }
        try {
            r.file = new VtParser(lr.toks).file();
        } catch (Fail f) {
            r.err = f.msg; r.line = f.line; r.col = f.col;
        }
        return r;
    }

    /* ---------- 基础 ---------- */
    private VtTok cur() { return t.get(pos); }
    private VtTok peek(int k) {
        int i = pos + k;
        return i < t.size() ? t.get(i) : t.get(t.size() - 1);   // EOF 兜底
    }
    private VtTok next() { VtTok x = cur(); if (pos < t.size() - 1) pos++; return x; }
    private boolean op(String s) { return cur().isOp(s); }
    private boolean kw(int k) { return cur().isKw(k); }
    private boolean sep() { return cur().kind == VtTok.SEP; }
    private void skipSeps() { while (sep()) next(); }

    private void fail(String word, String why) { fail(word, why, cur().line, cur().col); }
    private void fail(String word, String why, int l, int c) {
        throw new Fail(why == null ? word : word + "（" + why + "）", l, c);
    }

    private void expectOp(String s) {
        if (!op(s)) fail("语法错误", "缺 " + s);
        next();
    }

    /** 名字位：IDENT（含语境词）；保留字 → `保留字: <名>`。 */
    private String name(String what) {
        VtTok x = cur();
        if (x.kind == VtTok.IDENT) { next(); return x.text; }
        if (x.kind == VtTok.KW) fail("保留字: " + x.text, null, x.line, x.col);
        fail("语法错误", what + "需名字");
        return null;   // 不可达
    }

    /* ---------- 文件 / 块 ---------- */
    private VtAst.Block file() {
        VtAst.Block b = VtAst.file();
        skipSeps();
        while (cur().kind != VtTok.EOF) {
            b.sts.add(stmt());
            stmtEnd();
            skipSeps();
        }
        return b;
    }

    /** 块：`{ stmt* }`（允许 `{` 前换行）。 */
    private VtAst.Block block() {
        skipSeps();
        if (!op("{")) fail("语法错误", "缺 {");
        int bl = cur().line, bc = cur().col;
        next();
        VtAst.Block b = new VtAst.Block();
        b.line = bl; b.col = bc;
        b.sts = new ArrayList<VtAst.St>();
        skipSeps();
        while (!op("}")) {
            if (cur().kind == VtTok.EOF) fail("块未闭合", null, bl, bc);
            b.sts.add(stmt());
            stmtEnd();
            skipSeps();
        }
        b.endLine = cur().line;   // v36：闭括号行（注释归位）
        next();   // }
        return b;
    }

    /** 语句收尾：须接分隔 / `}` / EOF。 */
    private void stmtEnd() {
        if (sep() || op("}") || cur().kind == VtTok.EOF) return;
        if (kw(VtTok.K_FOUZE)) fail("否则不匹配", null);
        fail("语法错误", "缺语句分隔");
    }

    /* ---------- 语句分派 ---------- */
    private VtAst.St stmt() {
        VtTok x = cur();
        if (x.kind == VtTok.KW) {
            switch (x.kw) {
                case VtTok.K_DIANAN: case VtTok.K_HUADONG: case VtTok.K_DENGDAI:
                case VtTok.K_ANXIA: case VtTok.K_TANQI:
                    return action();
                case VtTok.K_RUGUO: return ifSt();
                case VtTok.K_XUNHUAN: return loop();
                case VtTok.K_DANG: case VtTok.K_ZHIDAO: return whileSt();
                case VtTok.K_TIAOCHU: case VtTok.K_JIXU: return ctl();
                case VtTok.K_ZHONGZHI: return term();
                case VtTok.K_FANHUI: return ret();
                case VtTok.K_TISHI: return toast();
                case VtTok.K_KUANGQUYU: return hl();
                case VtTok.K_KUANGJUXING: return box();
                case VtTok.K_HANSHU: return fn();
                case VtTok.K_BIAO: return table();
                case VtTok.K_TIAOZHUAN: return jmp();
                case VtTok.K_FOUZE:
                    fail("否则不匹配", null);
                    break;
                case VtTok.K_ZHAOTU: case VtTok.K_ZHAOSE: case VtTok.K_ZHAOSEDIAN:
                case VtTok.K_ZAIQUYU: case VtTok.K_KAIGUANKAI:
                case VtTok.K_LEN: case VtTok.K_STR: case VtTok.K_NUM: case VtTok.K_RAND:
                case VtTok.K_FLOOR: case VtTok.K_CEIL: case VtTok.K_ROUND: case VtTok.K_ABS:
                case VtTok.K_MIN: case VtTok.K_MAX: case VtTok.K_SQRT: case VtTok.K_SIN:
                case VtTok.K_COS: case VtTok.K_ATAN2: {
                    VtAst.CallSt s = new VtAst.CallSt();
                    s.line = x.line; s.col = x.col; s.call = callAfterName();
                    return s;
                }
                default:
                    fail("保留字: " + x.text, "不可作语句头", x.line, x.col);
                    break;
            }
        }
        if (x.kind == VtTok.IDENT) return identStmt();
        if (op("}")) fail("括号不匹配", "多余的 }");
        fail("语法错误", "记号 " + x.text, x.line, x.col);
        return null;   // 不可达
    }

    /** 点按/滑动/等待/按下/弹起。 */
    private VtAst.St action() {
        VtTok x = next();
        VtAst.ActionSt a = new VtAst.ActionSt();
        a.line = x.line; a.col = x.col; a.kw = x.kw;
        a.args = new ArrayList<VtAst.Ex>();
        a.opts = new ArrayList<VtAst.Opt>();
        int n;
        switch (x.kw) {
            case VtTok.K_DIANAN: n = 2; break;
            case VtTok.K_HUADONG: n = 4; break;
            case VtTok.K_DENGDAI: n = 1; break;
            case VtTok.K_ANXIA: n = 2; break;
            default: n = 0; break;   // 弹起
        }
        for (int i = 0; i < n; i++) a.args.add(expr());
        // 点按：第 3 位置参（按住时长 ms）可省；若后随 `键=` 则不是第 3 参
        if (x.kw == VtTok.K_DIANAN && exprStart()
                && !(cur().kind == VtTok.IDENT && peek(1).isOp("="))) a.args.add(expr());
        a.opts.addAll(opts());
        unknownKeyCheck();
        return a;
    }

    /** 命名参数序列（键=值；键 ∈ 语境词表）。 */
    private List<VtAst.Opt> opts() {
        List<VtAst.Opt> os = new ArrayList<VtAst.Opt>();
        while (true) {
            VtTok c = cur();
            if (c.kind == VtTok.IDENT && peek(1).isOp("=") && VtTok.isOptKey(c.text)) {
                next(); next();   // 键 =
                VtAst.Opt o = new VtAst.Opt();
                o.key = c.text; o.line = c.line; o.col = c.col;
                o.val = expr();
                os.add(o);
            } else break;
        }
        return os;
    }

    /** 选项之后若是 `名=` 但不是合法键 → 明确报错。 */
    private void unknownKeyCheck() {
        VtTok c = cur();
        if (c.kind == VtTok.IDENT && peek(1).isOp("=")) fail("语法错误", "未知参数键: " + c.text);
    }

    /** 当前位置是否可起一个表达式（用于“点按”可选第 3 参判定）。 */
    private boolean exprStart() {
        VtTok c = cur();
        if (c.kind == VtTok.NUM || c.kind == VtTok.STR || c.kind == VtTok.IDENT) return true;
        if (c.kind == VtTok.KW) return c.isKw(VtTok.K_ZHEN) || c.isKw(VtTok.K_JIA)
                || c.isKw(VtTok.K_LUN) || isCallableKw(c.kw);
        return c.isOp("(") || c.isOp("-") || c.isOp("!");
    }

    /** 如果/否则（否则允许跨换行，属同一语句）。 */
    private VtAst.St ifSt() {
        VtTok x = next();
        VtAst.IfSt s = new VtAst.IfSt();
        s.line = x.line; s.col = x.col;
        s.cond = expr();
        s.thenB = block();
        int save = pos;
        skipSeps();
        if (kw(VtTok.K_FOUZE)) { next(); s.elseB = block(); }
        else pos = save;   // 还原换行（留给外层语句分隔）
        return s;
    }

    /** 循环：4 形态（计数表达式 / 无限 / 次数=·时长= / for）。 */
    private VtAst.St loop() {
        VtTok x = next();
        VtAst.LoopSt s = new VtAst.LoopSt();
        s.line = x.line; s.col = x.col;
        s.opts = new ArrayList<VtAst.Opt>();
        if (kw(VtTok.K_WUXIAN)) {                       // 循环 无限
            next();
            s.mode = 1;
            s.body = block();
            return s;
        }
        if (cur().kind == VtTok.IDENT && peek(1).kind == VtTok.IDENT && "从".equals(peek(1).text)) {
            s.mode = 3;                                  // 循环 i 从 a 到 b
            s.var = name("循环变量");
            next();                                      // 从
            s.from = expr();
            if (!(cur().kind == VtTok.IDENT && "到".equals(cur().text))) fail("语法错误", "缺 到");
            next();
            s.to = expr();
            s.body = block();
            return s;
        }
        if (cur().kind == VtTok.IDENT && peek(1).isOp("=") && VtTok.isOptKey(cur().text)) {
            s.mode = 2;                                  // 循环 次数=3 / 时长=5000
            s.opts.addAll(opts());
            s.body = block();
            return s;
        }
        s.mode = 0;                                      // 循环 3 / 循环 n
        s.count = expr();
        s.body = block();
        return s;
    }

    /** 当 / 直到。 */
    private VtAst.St whileSt() {
        VtTok x = next();
        VtAst.WhileSt s = new VtAst.WhileSt();
        s.line = x.line; s.col = x.col;
        s.neg = x.kw == VtTok.K_ZHIDAO;
        s.cond = expr();
        s.body = block();
        return s;
    }

    /** 跳出 / 继续 [层数]。 */
    private VtAst.St ctl() {
        VtTok x = next();
        VtAst.CtlSt s = new VtAst.CtlSt();
        s.line = x.line; s.col = x.col; s.kw = x.kw; s.layers = 1;
        if (cur().kind == VtTok.NUM) {
            VtTok n = next();
            if (n.num != Math.floor(n.num) || n.num < 1 || n.num > 8) fail("语法错误", "层数 1..8", n.line, n.col);
            s.layers = (int) n.num;
        }
        return s;
    }

    /** 终止 [中止]。 */
    private VtAst.St term() {
        VtTok x = next();
        VtAst.TermSt s = new VtAst.TermSt();
        s.line = x.line; s.col = x.col;
        if (cur().kind == VtTok.IDENT && "中止".equals(cur().text)) { next(); s.abort = true; }
        return s;
    }

    /** 返回 [表达式]。 */
    private VtAst.St ret() {
        VtTok x = next();
        VtAst.RetSt s = new VtAst.RetSt();
        s.line = x.line; s.col = x.col;
        if (!sep() && !op("}") && cur().kind != VtTok.EOF) s.val = expr();
        return s;
    }

    /** 提示 表达式 [时长=]。 */
    private VtAst.St toast() {
        VtTok x = next();
        VtAst.ToastSt s = new VtAst.ToastSt();
        s.line = x.line; s.col = x.col;
        s.msg = expr();
        s.opts = opts();
        unknownKeyCheck();
        return s;
    }

    /** 框区域 "区域id" [时长=]（纯字符串字面量）。 */
    private VtAst.St hl() {
        VtTok x = next();
        VtAst.HlSt s = new VtAst.HlSt();
        s.line = x.line; s.col = x.col;
        VtTok c = cur();
        if (c.kind != VtTok.STR) fail("语法错误", "区域 id 需字符串字面量");
        if (c.segs.size() != 1 || !(c.segs.get(0) instanceof String)) fail("语法错误", "区域 id 需纯字符串（无插值）");
        next();
        s.region = (String) c.segs.get(0);
        s.opts = opts();
        unknownKeyCheck();
        return s;
    }

    /** 框矩形 x1 y1 x2 y2 [时长=]。 */
    private VtAst.St box() {
        VtTok x = next();
        VtAst.BoxSt s = new VtAst.BoxSt();
        s.line = x.line; s.col = x.col;
        s.x1 = expr(); s.y1 = expr(); s.x2 = expr(); s.y2 = expr();
        s.opts = opts();
        unknownKeyCheck();
        return s;
    }

    /** 函数 名(参数,…) { … }。 */
    private VtAst.St fn() {
        VtTok x = next();
        VtAst.FnSt s = new VtAst.FnSt();
        s.line = x.line; s.col = x.col;
        s.name = name("函数名");
        expectOp("(");
        s.params = new ArrayList<String>();
        if (!op(")")) {
            s.params.add(name("参数名"));
            while (op(",")) { next(); s.params.add(name("参数名")); }
        }
        expectOp(")");
        s.body = block();
        return s;
    }

    /** 表 名[长度]。 */
    private VtAst.St table() {
        VtTok x = next();
        VtAst.TableSt s = new VtAst.TableSt();
        s.line = x.line; s.col = x.col;
        s.name = name("表名");
        s.len = 64;
        if (op("[")) {
            next();
            VtTok n = cur();
            if (n.kind != VtTok.NUM) fail("表长度非法", "需整数字面量");
            next();
            if (n.num != Math.floor(n.num) || n.num < 1 || n.num > 4096) fail("表长度非法", "1..4096", n.line, n.col);
            s.len = (int) n.num;
            expectOp("]");
        }
        return s;
    }

    /** 跳转 第N行 / 跳转 结束（兼容语句）。 */
    private VtAst.St jmp() {
        VtTok x = next();
        VtAst.JmpSt s = new VtAst.JmpSt();
        s.line = x.line; s.col = x.col;
        VtTok c = cur();
        if (c.isKw(VtTok.K_JIESHU)) { next(); s.end = true; return s; }
        if (c.kind == VtTok.NUM) {
            next();
            if (c.num != Math.floor(c.num) || c.num < 1) fail("语法错误", "跳转 0 用 `跳转 结束`；行号 ≥1", c.line, c.col);
            s.target = (int) c.num;
            return s;
        }
        if (c.kind == VtTok.IDENT) {
            String digits = mLines(c.text);
            if (digits != null) {
                next();
                try { s.target = Integer.parseInt(digits); }
                catch (NumberFormatException e) { fail("语法错误", "跳转行号过大", c.line, c.col); }
                if (s.target < 1) fail("语法错误", "跳转行号 ≥1", c.line, c.col);
                return s;
            }
        }
        fail("语法错误", "跳转目标（第N行 / 结束）", c.line, c.col);
        return null;   // 不可达
    }

    /** `第12行` → "12"；不是该形态 → null。 */
    private static String mLines(String s) {
        if (s.length() < 3 || s.charAt(0) != '第' || s.charAt(s.length() - 1) != '行') return null;
        for (int i = 1; i < s.length() - 1; i++) if (s.charAt(i) < '0' || s.charAt(i) > '9') return null;
        return s.substring(1, s.length() - 1);
    }

    /** IDENT 起头的语句：调用 / 赋值。 */
    private VtAst.St identStmt() {
        VtTok id0 = cur();
        if (peek(1).isOp("(")) {
            VtAst.CallSt s = new VtAst.CallSt();
            s.line = id0.line; s.col = id0.col;
            s.call = callAfterName();
            return s;
        }
        next();
        VtAst.AssignSt a = new VtAst.AssignSt();
        a.line = id0.line; a.col = id0.col; a.name = id0.text;
        if (op("[")) {
            next();
            a.idx = expr();
            expectOp("]");
        }
        if (!op("=")) fail("语法错误", "赋值缺 =");
        next();
        a.val = expr();
        return a;
    }

    /** 调用（当前 token = 名/内建/视觉关键字）：名( arg[, arg]* )。 */
    private VtAst.CallEx callAfterName() {
        VtTok id = next();
        VtAst.CallEx c = new VtAst.CallEx();
        c.line = id.line; c.col = id.col;
        c.name = id.text;
        c.args = new ArrayList<VtAst.Ex>();
        c.opts = new ArrayList<VtAst.Opt>();
        expectOp("(");
        if (!op(")")) {
            arg(c);
            while (op(",")) { next(); arg(c); }
        }
        expectOp(")");
        return c;
    }

    private void arg(VtAst.CallEx c) {
        VtTok t0 = cur();
        if (t0.kind == VtTok.IDENT && peek(1).isOp("=") && VtTok.isOptKey(t0.text)) {
            next(); next();
            VtAst.Opt o = new VtAst.Opt();
            o.key = t0.text; o.line = t0.line; o.col = t0.col;
            o.val = expr();
            c.opts.add(o);
        } else {
            if (t0.kind == VtTok.IDENT && peek(1).isOp("=")) fail("语法错误", "未知参数键: " + t0.text);
            c.args.add(expr());
        }
    }

    /* ---------- 表达式（优先级爬升） ---------- */
    private VtAst.Ex expr() { return or(); }

    private VtAst.Ex or() {
        VtAst.Ex l = and();
        while (op("||")) { VtTok o = next(); l = bin(o, "||", l, and()); }
        return l;
    }
    private VtAst.Ex and() {
        VtAst.Ex l = cmp();
        while (op("&&")) { VtTok o = next(); l = bin(o, "&&", l, cmp()); }
        return l;
    }
    private VtAst.Ex cmp() {
        VtAst.Ex l = add();
        if (op("==") || op("!=") || op("<") || op(">") || op("<=") || op(">=")) {
            VtTok o = next();
            VtAst.Ex r = add();
            if (op("==") || op("!=") || op("<") || op(">") || op("<=") || op(">=")) fail("语法错误", "比较不可连写");
            return bin(o, o.text, l, r);
        }
        return l;
    }
    private VtAst.Ex add() {
        VtAst.Ex l = mul();
        while (op("+") || op("-")) { VtTok o = next(); l = bin(o, o.text, l, mul()); }
        return l;
    }
    private VtAst.Ex mul() {
        VtAst.Ex l = unary();
        while (op("*") || op("/") || op("%")) { VtTok o = next(); l = bin(o, o.text, l, unary()); }
        return l;
    }
    private VtAst.Ex unary() {
        if (op("-") || op("!")) {
            VtTok o = next();
            VtAst.UnEx u = new VtAst.UnEx();
            u.line = o.line; u.col = o.col; u.op = o.text;
            u.e = unary();
            return u;
        }
        return prim();
    }

    private VtAst.BinEx bin(VtTok o, String s, VtAst.Ex l, VtAst.Ex r) {
        VtAst.BinEx b = new VtAst.BinEx();
        b.line = o.line; b.col = o.col; b.op = s; b.l = l; b.r = r;
        return b;
    }

    private VtAst.Ex prim() {
        VtTok c = cur();
        if (c.kind == VtTok.NUM) {
            next();
            VtAst.NumEx n = new VtAst.NumEx();
            n.line = c.line; n.col = c.col; n.v = c.num; n.raw = c.text;
            return n;
        }
        if (c.kind == VtTok.STR) {
            next();
            VtAst.StrEx s = new VtAst.StrEx();
            s.line = c.line; s.col = c.col; s.raw = c.raw; s.segs = c.segs;
            return s;
        }
        if (c.isKw(VtTok.K_ZHEN) || c.isKw(VtTok.K_JIA) || c.isKw(VtTok.K_LUN)) {
            next();
            VtAst.LitEx l = new VtAst.LitEx();
            l.line = c.line; l.col = c.col; l.kw = c.kw;
            return l;
        }
        if (c.kind == VtTok.KW && isCallableKw(c.kw)) {
            if (!peek(1).isOp("(")) fail("语法错误", "内建/视觉函数需调用: " + c.text, c.line, c.col);
            return callAfterName();
        }
        if (c.kind == VtTok.IDENT) {
            if (peek(1).isOp("(")) return callAfterName();
            if (peek(1).isOp("[")) {
                next(); next();   // 名 [
                VtAst.IdxEx x = new VtAst.IdxEx();
                x.line = c.line; x.col = c.col; x.name = c.text;
                x.idx = expr();
                expectOp("]");
                return x;
            }
            next();
            VtAst.IdEx x = new VtAst.IdEx();
            x.line = c.line; x.col = c.col; x.name = c.text;
            return x;
        }
        if (op("(")) {
            next();
            VtAst.Ex e = expr();
            expectOp(")");
            return e;
        }
        if (sep() || c.kind == VtTok.EOF || op("}") || op(")") || op("]") || op(",") || op("{"))
            fail("表达式为空", null, c.line, c.col);
        fail("语法错误", "记号 " + c.text, c.line, c.col);
        return null;   // 不可达
    }

    private static boolean isCallableKw(int k) {
        switch (k) {
            case VtTok.K_ZHAOTU: case VtTok.K_ZHAOSE: case VtTok.K_ZHAOSEDIAN:
            case VtTok.K_ZAIQUYU: case VtTok.K_KAIGUANKAI:
            case VtTok.K_LEN: case VtTok.K_STR: case VtTok.K_NUM: case VtTok.K_RAND:
            case VtTok.K_FLOOR: case VtTok.K_CEIL: case VtTok.K_ROUND: case VtTok.K_ABS:
            case VtTok.K_MIN: case VtTok.K_MAX: case VtTok.K_SQRT: case VtTok.K_SIN:
            case VtTok.K_COS: case VtTok.K_ATAN2:
                return true;
            default:
                return false;
        }
    }
}
