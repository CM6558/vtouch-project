package com.vtouch.editor;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** vt-script 静态检查（T1.3）：名字解析 / 上下文 / 参数匹配 / 上限 / 静态类型。
 *  规范 §3（参数键支持集）/ §5（作用域与解析顺序）/ §6（编译期词表全量）。纯 Java、零 Android 依赖。 */
public final class VtCheck {

    /** 检查结果：err == null 时通过；sym = 符号表（T2 编译器复用）。 */
    public static final class R {
        public String err;
        public int line = 1, col = 1;
        public Sym sym;
    }

    /** 符号表：全局变量 / 表（名→长度）/ 函数（名→形参数）。 */
    public static final class Sym {
        public final Map<String, Integer> globals = new HashMap<String, Integer>();
        public final Map<String, Integer> tables = new HashMap<String, Integer>();
        public final Map<String, Integer> fns = new HashMap<String, Integer>();
    }

    /** 预定义变量（读写皆可；规范 §5 r4）：触发点/抬起点/时刻/命中坐标。 */
    public static final String[] PRE_VARS = {"tdx", "tdy", "tux", "tuy", "tms", "fx", "fy"};

    private static final class Fail extends RuntimeException {
        final String msg; final int line, col;
        Fail(String m, int l, int c) { super(m); msg = m; line = l; col = c; }
    }

    private static void fail(String word, String why, int l, int c) {
        throw new Fail(why == null ? word : word + "（" + why + "）", l, c);
    }

    /** 全链便捷入口：过大 → 词法 → 语法 → 静态检查。 */
    public static R run(String src) {
        R r = new R();
        try {
            String s = src == null ? "" : src;
            if (utf8Len(s) > 65536) fail("文件过大", ">64KB", 1, 1);
            VtParser.R pr = VtParser.parse(s);
            if (pr.err != null) { r.err = pr.err; r.line = pr.line; r.col = pr.col; return r; }
            Sym sym = new Sym();
            new VtCheck(sym).checkFile(pr.file);
            r.sym = sym;
        } catch (Fail f) {
            r.err = f.msg; r.line = f.line; r.col = f.col;
        }
        return r;
    }

    private static int utf8Len(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else n += 3;
        }
        return n;
    }

    /* ================= 实例 ================= */

    private final Sym sym;
    private final Map<String, String> declKind = new HashMap<String, String>();   // 名 → G/T/F
    private Map<Integer, VtAst.St> topStarts = new HashMap<Integer, VtAst.St>();  // 顶层语句起始行 → 语句
    private final Map<String, Set<String>> fnLocals = new HashMap<String, Set<String>>();   // 函数帧局部（参数+计数变量）
    private final Set<String> fileLocals = new HashSet<String>();                           // 顶层帧局部（计数变量）

    private VtCheck(Sym sym) {
        this.sym = sym;
        for (String p : PRE_VARS) {
            declKind.put(p, "G");
            sym.globals.put(p, 0);
        }
    }

    private void checkFile(VtAst.Block f) {
        declPass(f, fileLocals);                     // A1：表/函数声明 + 帧局部（计数变量）+ 冲突
        assignPass(f, fileLocals);                   // A2：赋值 → 全局变量 + 冲突
        for (VtAst.St s : f.sts) topStarts.put(s.line, s);
        Ctx ctx = new Ctx();
        ctx.locals = fileLocals;                     // 帧局部 = 参数 + 计数变量（帧内全程可见）
        resolveBlock(f, ctx);                        // Pass B：解析 + 上下文 + 类型
    }

    /** 声明冲突（G 重复=合法；跨类/同表同名/同函数名 → 重名）。 */
    private void decl(String name, String kind, int l, int c) {
        if (name.length() > 63) fail("名字过长", ">63 字符", l, c);
        String prev = declKind.get(name);
        if (prev != null) {
            if (prev.equals(kind) && kind.equals("G")) return;
            fail("重名: " + name, kindName(prev) + " 已声明", l, c);
        }
        declKind.put(name, kind);
    }

    private static String kindName(String k) {
        if (k.equals("T")) return "表";
        if (k.equals("F")) return "函数";
        return "变量";
    }

    /* ---------- Pass A1：表/函数声明 + 帧局部（计数变量） ---------- */
    private void declPass(VtAst.Block b, Set<String> frameLocals) {
        for (VtAst.St s : b.sts) {
            if (s instanceof VtAst.TableSt) {
                VtAst.TableSt t = (VtAst.TableSt) s;
                decl(t.name, "T", t.line, t.col);
                sym.tables.put(t.name, t.len);
                continue;
            }
            if (s instanceof VtAst.FnSt) {
                VtAst.FnSt f = (VtAst.FnSt) s;
                decl(f.name, "F", f.line, f.col);
                if (f.params.size() > 32) fail("参数不匹配", "参数 ≤32", f.line, f.col);
                Set<String> seen = new HashSet<String>();
                for (String p : f.params) {
                    if (p.length() > 63) fail("名字过长", ">63 字符", f.line, f.col);
                    if (!seen.add(p)) fail("重名: " + p, "形参重复", f.line, f.col);
                }
                sym.fns.put(f.name, f.params.size());
                Set<String> fl = new HashSet<String>(f.params);
                fnLocals.put(f.name, fl);
                declPass(f.body, fl);
                continue;
            }
            if (s instanceof VtAst.LoopSt) {
                VtAst.LoopSt l = (VtAst.LoopSt) s;
                if (l.mode == 3) {
                    if (l.var.length() > 63) fail("名字过长", ">63 字符", l.line, l.col);
                    frameLocals.add(l.var);          // 帧局部：循环结束后仍可见（规范示例 2 口径）
                }
                declPass(l.body, frameLocals);
                continue;
            }
            if (s instanceof VtAst.IfSt) {
                VtAst.IfSt f = (VtAst.IfSt) s;
                declPass(f.thenB, frameLocals);
                if (f.elseB != null) declPass(f.elseB, frameLocals);
                continue;
            }
            if (s instanceof VtAst.WhileSt) {
                declPass(((VtAst.WhileSt) s).body, frameLocals);
                continue;
            }
            // 其余语句无声明
        }
    }

    /* ---------- Pass A2：赋值 → 全局变量（跳过帧局部） ---------- */
    private void assignPass(VtAst.Block b, Set<String> frameLocals) {
        for (VtAst.St s : b.sts) {
            if (s instanceof VtAst.AssignSt) {
                VtAst.AssignSt a = (VtAst.AssignSt) s;
                if (a.idx == null && !frameLocals.contains(a.name)) {
                    decl(a.name, "G", a.line, a.col);
                    if (!sym.globals.containsKey(a.name)) sym.globals.put(a.name, a.line);
                }
                continue;
            }
            if (s instanceof VtAst.FnSt) {
                VtAst.FnSt f = (VtAst.FnSt) s;
                assignPass(f.body, fnLocals.get(f.name));
                continue;
            }
            if (s instanceof VtAst.LoopSt) {
                assignPass(((VtAst.LoopSt) s).body, frameLocals);
                continue;
            }
            if (s instanceof VtAst.IfSt) {
                VtAst.IfSt f = (VtAst.IfSt) s;
                assignPass(f.thenB, frameLocals);
                if (f.elseB != null) assignPass(f.elseB, frameLocals);
                continue;
            }
            if (s instanceof VtAst.WhileSt) {
                assignPass(((VtAst.WhileSt) s).body, frameLocals);
                continue;
            }
        }
    }

    /* ---------- Pass B：解析与上下文 ---------- */

    /** 作用域上下文（词法）。 */
    private static final class Ctx {
        Set<String> locals = new HashSet<String>();
        int loopDepth;
        int depth;          // 块深（含函数体）
        boolean inFn;
    }

    private void resolveBlock(VtAst.Block b, Ctx ctx) {
        for (VtAst.St s : b.sts) resolveSt(s, ctx);
    }

    /** 进块（深 +1，嵌套过深检查）。 */
    private void enterBlock(VtAst.Block b, Ctx ctx) {
        if (ctx.depth + 1 > 32) fail("嵌套过深", ">32", b.line, b.col);
        ctx.depth++;
    }

    private void resolveSt(VtAst.St s, Ctx ctx) {
        if (s instanceof VtAst.ActionSt) {
            VtAst.ActionSt a = (VtAst.ActionSt) s;
            for (VtAst.Ex e : a.args) resolveEx(e, ctx);
            if (a.kw == VtTok.K_HUADONG) optKeys(a.opts, K_滑动, "滑动");
            else if (!a.opts.isEmpty()) fail("参数不匹配", VtTok.KW_NAMES[a.kw - 1] + " 不支持键: " + a.opts.get(0).key, a.opts.get(0).line, a.opts.get(0).col);
            for (VtAst.Opt o : a.opts) resolveEx(o.val, ctx);
            return;
        }
        if (s instanceof VtAst.AssignSt) {
            VtAst.AssignSt a = (VtAst.AssignSt) s;
            if (a.idx != null) {
                checkTableName(a.name, a.line, a.col, ctx);
                resolveEx(a.idx, ctx);
            }
            resolveEx(a.val, ctx);
            return;
        }
        if (s instanceof VtAst.IfSt) {
            VtAst.IfSt f = (VtAst.IfSt) s;
            resolveCond(f.cond, ctx);
            enterBlock(f.thenB, ctx); resolveBlock(f.thenB, ctx); ctx.depth--;
            if (f.elseB != null) { enterBlock(f.elseB, ctx); resolveBlock(f.elseB, ctx); ctx.depth--; }
            return;
        }
        if (s instanceof VtAst.LoopSt) {
            VtAst.LoopSt l = (VtAst.LoopSt) s;
            if (l.mode == 0) resolveEx(l.count, ctx);
            if (l.mode == 2) {
                optKeys(l.opts, K_循环, "循环");
                if (l.opts.size() > 1) fail("参数不匹配", "循环模式键至多一个", l.opts.get(1).line, l.opts.get(1).col);
                for (VtAst.Opt o : l.opts) resolveEx(o.val, ctx);
            }
            if (l.mode == 3) { resolveEx(l.from, ctx); resolveEx(l.to, ctx); }
            enterBlock(l.body, ctx);
            ctx.loopDepth++;
            resolveBlock(l.body, ctx);              // 计数变量已在帧局部集（A1，帧内全程可见）
            ctx.loopDepth--;
            ctx.depth--;
            return;
        }
        if (s instanceof VtAst.WhileSt) {
            VtAst.WhileSt w = (VtAst.WhileSt) s;
            resolveCond(w.cond, ctx);
            enterBlock(w.body, ctx);
            ctx.loopDepth++;
            resolveBlock(w.body, ctx);
            ctx.loopDepth--;
            ctx.depth--;
            return;
        }
        if (s instanceof VtAst.CtlSt) {
            VtAst.CtlSt c = (VtAst.CtlSt) s;
            if (ctx.loopDepth == 0) fail("跳出在循环外", VtTok.KW_NAMES[c.kw - 1], c.line, c.col);
            if (c.layers > ctx.loopDepth) fail("跳出在循环外", "层数 " + c.layers + " > 嵌套 " + ctx.loopDepth, c.line, c.col);
            return;
        }
        if (s instanceof VtAst.TermSt) return;
        if (s instanceof VtAst.RetSt) {
            VtAst.RetSt r = (VtAst.RetSt) s;
            if (!ctx.inFn) fail("返回在函数外", null, r.line, r.col);
            if (r.val != null) resolveEx(r.val, ctx);
            return;
        }
        if (s instanceof VtAst.ToastSt) {
            VtAst.ToastSt t = (VtAst.ToastSt) s;
            resolveEx(t.msg, ctx);
            optKeys(t.opts, K_时长, "提示");
            for (VtAst.Opt o : t.opts) resolveEx(o.val, ctx);
            return;
        }
        if (s instanceof VtAst.HlSt) {
            VtAst.HlSt h = (VtAst.HlSt) s;
            optKeys(h.opts, K_时长, "框区域");
            for (VtAst.Opt o : h.opts) resolveEx(o.val, ctx);
            return;
        }
        if (s instanceof VtAst.BoxSt) {
            VtAst.BoxSt x = (VtAst.BoxSt) s;
            resolveEx(x.x1, ctx); resolveEx(x.y1, ctx); resolveEx(x.x2, ctx); resolveEx(x.y2, ctx);
            optKeys(x.opts, K_时长, "框矩形");
            for (VtAst.Opt o : x.opts) resolveEx(o.val, ctx);
            return;
        }
        if (s instanceof VtAst.FnSt) {
            VtAst.FnSt f = (VtAst.FnSt) s;
            if (ctx.depth > 0 || ctx.inFn) fail("函数嵌套", "函数仅限顶层", f.line, f.col);
            Ctx fc = new Ctx();
            fc.inFn = true;
            fc.locals = fnLocals.get(f.name);       // 帧局部 = 参数 + 计数变量（A1 全集）
            fc.depth = ctx.depth + 1;
            if (fc.depth > 32) fail("嵌套过深", ">32", f.line, f.col);
            resolveBlock(f.body, fc);
            return;
        }
        if (s instanceof VtAst.TableSt) {
            if (ctx.depth > 0) fail("语法错误", "表定义仅限顶层", s.line, s.col);
            return;
        }
        if (s instanceof VtAst.CallSt) {
            resolveEx(((VtAst.CallSt) s).call, ctx);
            return;
        }
        if (s instanceof VtAst.JmpSt) {
            VtAst.JmpSt j = (VtAst.JmpSt) s;
            if (ctx.depth > 0) fail("跳转跨越块边界", null, j.line, j.col);
            if (j.end) return;
            VtAst.St tgt = topStarts.get(j.target);
            if (tgt == null) fail("语法错误", "跳转目标非语句行: 第" + j.target + "行", j.line, j.col);
            if (tgt == j) fail("语法错误", "跳转目标为自身", j.line, j.col);
            return;
        }
    }

    private static boolean isLitStr(VtAst.Ex e) {
        if (!(e instanceof VtAst.StrEx)) return false;
        VtAst.StrEx s = (VtAst.StrEx) e;
        return s.segs.size() == 1 && (s.segs.get(0) instanceof String);
    }

    private void resolveCond(VtAst.Ex cond, Ctx ctx) {
        resolveEx(cond, ctx);
        if (staticType(cond) == T_STR) fail("类型错", "条件为字符串", cond.line, cond.col);
    }

    /** 索引/表写目标必须是表（帧局部也算"不是表"）。 */
    private void checkTableName(String name, int l, int c, Ctx ctx) {
        if (sym.tables.containsKey(name)) return;
        if (ctx.locals.contains(name) || declKind.containsKey(name)) fail("语法错误", "不是表: " + name, l, c);
        fail("未定义: " + name, null, l, c);
    }

    private void optKeys(List<VtAst.Opt> opts, Set<String> allowed, String what) {
        Set<String> seen = new HashSet<String>();
        for (VtAst.Opt o : opts) {
            if (!allowed.contains(o.key)) fail("参数不匹配", what + " 不支持键: " + o.key, o.line, o.col);
            if (!seen.add(o.key)) fail("参数不匹配", what + " 重复键: " + o.key, o.line, o.col);
        }
    }

    /* ---------- 表达式 ---------- */

    private void resolveEx(VtAst.Ex e, Ctx ctx) {
        if (e instanceof VtAst.NumEx) return;
        if (e instanceof VtAst.StrEx) { resolveStr((VtAst.StrEx) e, ctx); return; }
        if (e instanceof VtAst.LitEx) {
            VtAst.LitEx l = (VtAst.LitEx) e;
            if (l.kw == VtTok.K_LUN && ctx.loopDepth == 0) fail("循环外引用 轮", null, l.line, l.col);
            return;
        }
        if (e instanceof VtAst.IdEx) {
            VtAst.IdEx x = (VtAst.IdEx) e;
            if (ctx.locals.contains(x.name) || sym.globals.containsKey(x.name)) return;
            if (sym.tables.containsKey(x.name)) fail("语法错误", "表名需下标: " + x.name, x.line, x.col);
            fail("未定义: " + x.name, null, x.line, x.col);
        }
        if (e instanceof VtAst.IdxEx) {
            VtAst.IdxEx x = (VtAst.IdxEx) e;
            checkTableName(x.name, x.line, x.col, ctx);
            resolveEx(x.idx, ctx);
            return;
        }
        if (e instanceof VtAst.CallEx) { resolveCall((VtAst.CallEx) e, ctx); return; }
        if (e instanceof VtAst.BinEx) {
            VtAst.BinEx b = (VtAst.BinEx) e;
            resolveEx(b.l, ctx);
            resolveEx(b.r, ctx);
            int tl = staticType(b.l), tr = staticType(b.r);
            if (b.op.equals("+")) return;
            if (b.op.equals("&&") || b.op.equals("||")) {
                if (tl == T_STR || tr == T_STR) fail("类型错", "逻辑运算遇字符串", b.line, b.col);
                return;
            }
            if (b.op.equals("==") || b.op.equals("!=")) return;
            if (tl == T_STR || tr == T_STR) fail("类型错", "大小比较遇字符串", b.line, b.col);
            return;
        }
        if (e instanceof VtAst.UnEx) {
            VtAst.UnEx u = (VtAst.UnEx) e;
            resolveEx(u.e, ctx);
            if (staticType(u.e) == T_STR) fail("类型错", "一元 " + u.op + " 遇字符串", u.line, u.col);
            return;
        }
    }

    private void resolveStr(VtAst.StrEx s, Ctx ctx) {
        for (Object o : s.segs) {
            if (!(o instanceof VtTok.Interp)) continue;
            VtTok.Interp ip = (VtTok.Interp) o;
            String nm = ip.name;
            if (nm.equals("轮")) {
                if (ctx.loopDepth == 0) fail("循环外引用 轮", "字符串插值", ip.line, ip.col);
                continue;
            }
            if (nm.equals("真") || nm.equals("假")) continue;
            if (VtTok.kwOf(nm) > 0) fail("保留字: " + nm, null, ip.line, ip.col);
            boolean ok = ctx.locals.contains(nm) || sym.globals.containsKey(nm) || sym.tables.containsKey(nm);
            if (!ok) fail("未定义: " + nm, null, ip.line, ip.col);
            if (ip.hasIdx) {
                if (!sym.tables.containsKey(nm)) fail("语法错误", "不是表: " + nm, ip.line, ip.col);
                if (ip.idxName != null && !ip.idxName.equals("轮")) {
                    if (!ctx.locals.contains(ip.idxName) && !sym.globals.containsKey(ip.idxName)
                            && !sym.tables.containsKey(ip.idxName))
                        fail("未定义: " + ip.idxName, null, ip.line, ip.col);
                }
            }
        }
    }

    private void resolveCall(VtAst.CallEx c, Ctx ctx) {
        Integer fna = sym.fns.get(c.name);
        if (fna != null) {
            if (c.opts.size() > 0) fail("参数不匹配", "用户函数不支持命名实参: " + c.name, c.line, c.col);
            if (c.args.size() != fna.intValue()) fail("参数不匹配", "实参 " + c.args.size() + " ≠ 形参 " + fna, c.line, c.col);
            for (VtAst.Ex a : c.args) resolveEx(a, ctx);
            return;
        }
        Integer bna = FN_ARITY.get(c.name);
        if (bna != null) {
            if (c.args.size() != bna.intValue()) fail("参数不匹配", c.name + "(" + bna + " 位)", c.line, c.col);
            Set<String> keys = FN_KEYS.get(c.name);
            optKeys(c.opts, keys == null ? K_NONE : keys, c.name);
            if (c.name.equals("找图") || c.name.equals("找色") || c.name.equals("在区域") || c.name.equals("开关开")) {
                VtAst.Ex a0 = c.args.get(0);
                if (!isLitStr(a0)) fail("参数不匹配", c.name + " 首参需字符串字面量", a0.line, a0.col);
            }
            for (int i = 0; i < c.opts.size(); i++) {
                VtAst.Opt o = c.opts.get(i);
                if (o.key.equals("区域") && !isLitStr(o.val)) fail("参数不匹配", "区域需字符串字面量", o.val.line, o.val.col);
            }
            for (VtAst.Ex a : c.args) resolveEx(a, ctx);
            for (VtAst.Opt o : c.opts) resolveEx(o.val, ctx);
            return;
        }
        fail("函数未定义: " + c.name, null, c.line, c.col);
    }

    /* ---------- 静态类型（可判部分） ---------- */

    private static final int T_UNKN = 0, T_NUM = 1, T_STR = 2;
    private static final Set<String> STR_RET = set("str");

    private int staticType(VtAst.Ex e) {
        if (e instanceof VtAst.StrEx) return T_STR;
        if (e instanceof VtAst.NumEx || e instanceof VtAst.LitEx) return T_NUM;
        if (e instanceof VtAst.CallEx) {
            VtAst.CallEx c = (VtAst.CallEx) e;
            if (sym.fns.containsKey(c.name)) return T_UNKN;
            return STR_RET.contains(c.name) ? T_STR : T_NUM;   // 内建/视觉返回数值
        }
        if (e instanceof VtAst.BinEx) {
            VtAst.BinEx b = (VtAst.BinEx) e;
            int tl = staticType(b.l), tr = staticType(b.r);
            if (b.op.equals("+")) {
                if (tl == T_STR || tr == T_STR) return T_STR;
                return (tl == T_UNKN || tr == T_UNKN) ? T_UNKN : T_NUM;
            }
            if (b.op.equals("-") || b.op.equals("*") || b.op.equals("/") || b.op.equals("%"))
                return (tl == T_UNKN || tr == T_UNKN) ? T_UNKN : T_NUM;
            return T_NUM;                                       // 比较/逻辑 → 1/0
        }
        if (e instanceof VtAst.UnEx) return T_NUM;
        return T_UNKN;                                          // 变量 / 表读 / …
    }

    /* ---------- 参数键表 / 函数表 ---------- */

    private static final Set<String> K_NONE = new HashSet<String>();
    private static final Set<String> K_时长 = set("时长");
    private static final Set<String> K_滑动 = set("时长", "停1", "停2");
    private static final Set<String> K_循环 = set("次数", "时长");
    private static final Set<String> K_找图 = set("区域", "阈值", "色校", "持续");
    private static final Set<String> K_找色 = set("区域");
    private static final Set<String> K_找色点 = set("容差", "区域");

    private static final Map<String, Integer> FN_ARITY = new HashMap<String, Integer>();
    private static final Map<String, Set<String>> FN_KEYS = new HashMap<String, Set<String>>();

    static {
        FN_ARITY.put("len", 1); FN_ARITY.put("str", 1); FN_ARITY.put("num", 1);
        FN_ARITY.put("rand", 2); FN_ARITY.put("floor", 1); FN_ARITY.put("ceil", 1);
        FN_ARITY.put("round", 1); FN_ARITY.put("abs", 1); FN_ARITY.put("min", 2);
        FN_ARITY.put("max", 2); FN_ARITY.put("sqrt", 1); FN_ARITY.put("sin", 1);
        FN_ARITY.put("cos", 1); FN_ARITY.put("atan2", 2);
        FN_ARITY.put("找图", 1); FN_ARITY.put("找色", 1); FN_ARITY.put("找色点", 1);
        FN_ARITY.put("在区域", 3); FN_ARITY.put("开关开", 1);
        FN_KEYS.put("找图", K_找图); FN_KEYS.put("找色", K_找色); FN_KEYS.put("找色点", K_找色点);
    }

    private static Set<String> set(String... xs) {
        Set<String> s = new HashSet<String>();
        for (String x : xs) s.add(x);
        return s;
    }
}
