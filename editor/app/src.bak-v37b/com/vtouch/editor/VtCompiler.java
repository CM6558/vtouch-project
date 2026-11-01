package com.vtouch.editor;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** vt-script 编译器（T2.2）：已检查 AST → VTBC v1 字节码。格式见 docs/VTBC_V1.md。
 *  纯 Java、零 Android 依赖。入口 compile(src)：内部走 VtCheck 全链；首错即返。
 *  语义要点：短路=跳转编译；for 计数变量正常跑完归位=上限值；轮=帧内迭代序号（0 起）；
 *  跳出/继续 层数 → 目标循环标签；兼容跳转 → CJMP；语句位调用结果 POP 弃。 */
public final class VtCompiler {

    public static final class R {
        public VtBc.Blob blob;
        public String err;
        public int line = 1, col = 1;
    }

    /** 全链编译：词法→语法→检查→字节码（含 32KB 写端自检）。 */
    public static R compile(String src) {
        R r = new R();
        VtCheck.R c = VtCheck.run(src);
        if (c.err != null) { r.err = c.err; r.line = c.line; r.col = c.col; return r; }
        VtParser.R p = VtParser.parse(src);
        try {
            r.blob = new VtCompiler(c.sym, VtBc.hash(src)).run(p.file);
            VtBc.write(r.blob);                       // 写端自检（含 程序过大）
        } catch (IllegalArgumentException e) {
            r.err = e.getMessage(); r.blob = null;
        } catch (IllegalStateException e) {
            r.err = "编译器内部: " + e.getMessage(); r.blob = null;
        }
        return r;
    }

    /* ---------- 实例状态 ---------- */

    private final VtCheck.Sym sym;
    private final int srcHash;
    private final Map<String, Integer> globals = new HashMap<String, Integer>();
    private final Map<String, Integer> tables = new HashMap<String, Integer>();
    private final Map<String, Integer> fnIdx = new HashMap<String, Integer>();
    private final List<String> fnOrder = new ArrayList<String>();
    private final List<VtBc.Const> consts = new ArrayList<VtBc.Const>();
    private final Map<Long, Integer> numPool = new HashMap<Long, Integer>();
    private final Map<String, Integer> strPool = new HashMap<String, Integer>();
    private final List<VtBc.Line> lines = new ArrayList<VtBc.Line>();
    private final List<VtBc.Name> names = new ArrayList<VtBc.Name>();
    private final Map<Integer, Label> jmpLabels = new HashMap<Integer, Label>();
    private byte[] buf = new byte[512];
    private int p;
    private int lastLine = -1;
    private int slotBase;                              // 7 + 用户全局数（局部槽寻址基址）

    /* 当前帧 */
    private Map<String, Integer> locals = new HashMap<String, Integer>();
    private int localCount;
    private final List<Loop> loopStack = new ArrayList<Loop>();

    private static final class Label {
        final List<Integer> sites = new ArrayList<Integer>();
        int off = -1;
    }
    private static final class Loop {
        final Label brk = new Label();
        final Label cont = new Label();
        int round;
    }

    private VtCompiler(VtCheck.Sym sym, int srcHash) {
        this.sym = sym; this.srcHash = srcHash;
    }

    /* ---------- 写缓冲 ---------- */
    private void ensure(int n) {
        if (p + n > buf.length) {
            byte[] nb = new byte[Math.max(buf.length * 2, p + n)];
            System.arraycopy(buf, 0, nb, 0, p);
            buf = nb;
        }
    }
    private void w8(int v) { ensure(1); buf[p++] = (byte) v; }
    private void w32v(int v) { ensure(4); buf[p++] = (byte) v; buf[p++] = (byte) (v >>> 8); buf[p++] = (byte) (v >>> 16); buf[p++] = (byte) (v >>> 24); }
    private void patch32(int at, int v) { buf[at] = (byte) v; buf[at + 1] = (byte) (v >>> 8); buf[at + 2] = (byte) (v >>> 16); buf[at + 3] = (byte) (v >>> 24); }
    private void place(Label l) { l.off = p; for (int i = 0; i < l.sites.size(); i++) patch32(l.sites.get(i).intValue(), p); l.sites.clear(); }
    private void jmp(Label l) { w8(VtOp.JMP); fix(l); }
    private void jf(Label l) { w8(VtOp.JF); fix(l); }
    private void jt(Label l) { w8(VtOp.JT); fix(l); }
    private void cjmp(Label l) { w8(VtOp.CJMP); fix(l); }
    /** 跳转操作数：已落位（回跳）立即写入；否则挂补丁位。 */
    private void fix(Label l) {
        if (l.off >= 0) w32v(l.off);
        else { l.sites.add(Integer.valueOf(p)); w32v(0); }
    }

    /* ---------- 常量池 ---------- */
    private int constNum(double v) {
        Long k = Long.valueOf(Double.doubleToLongBits(v));
        Integer i = numPool.get(k);
        if (i != null) return i.intValue();
        VtBc.Const c = new VtBc.Const(); c.kind = 0; c.num = v;
        consts.add(c);
        int ix = consts.size() - 1;
        numPool.put(k, Integer.valueOf(ix));
        return ix;
    }
    private int constStr(String s) {
        Integer i = strPool.get(s);
        if (i != null) return i.intValue();
        VtBc.Const c = new VtBc.Const(); c.kind = 1; c.str = s;
        consts.add(c);
        int ix = consts.size() - 1;
        strPool.put(s, Integer.valueOf(ix));
        return ix;
    }
    private void pushNum(double v) { w8(VtOp.KN); w32v(constNum(v)); }

    /* ---------- 主流程 ---------- */

    private VtBc.Blob run(VtAst.Block file) {
        // 槽位/索引分配（确定性）
        int slot = 7;
        globals.put("tdx", Integer.valueOf(0)); globals.put("tdy", Integer.valueOf(1));
        globals.put("tux", Integer.valueOf(2)); globals.put("tuy", Integer.valueOf(3));
        globals.put("tms", Integer.valueOf(4)); globals.put("fx", Integer.valueOf(5));
        globals.put("fy", Integer.valueOf(6));
        final List<String> gs = new ArrayList<String>();
        for (String g : sym.globals.keySet()) if (!globals.containsKey(g)) gs.add(g);
        Collections.sort(gs, new Comparator<String>() {
            public int compare(String a, String b) {
                int la = sym.globals.get(a) == null ? 0 : sym.globals.get(a).intValue();
                int lb = sym.globals.get(b) == null ? 0 : sym.globals.get(b).intValue();
                if (la != lb) return la - lb;
                return a.compareTo(b);
            }
        });
        for (int i = 0; i < gs.size(); i++) globals.put(gs.get(i), Integer.valueOf(slot++));
        slotBase = slot;
        final List<String> ts = new ArrayList<String>(sym.tables.keySet());
        Collections.sort(ts);
        for (int i = 0; i < ts.size(); i++) tables.put(ts.get(i), Integer.valueOf(i));
        final List<String> fs = new ArrayList<String>(sym.fns.keySet());
        Collections.sort(fs);
        for (int i = 0; i < fs.size(); i++) { fnOrder.add(fs.get(i)); fnIdx.put(fs.get(i), Integer.valueOf(i)); }
        Map<String, VtAst.FnSt> fnAst = new HashMap<String, VtAst.FnSt>();
        for (int i = 0; i < file.sts.size(); i++) {
            if (file.sts.get(i) instanceof VtAst.FnSt) {
                VtAst.FnSt f = (VtAst.FnSt) file.sts.get(i);
                fnAst.put(f.name, f);
            }
        }
        // 常量 0/1 预置（短路结果等）
        constNum(0); constNum(1);

        // 跳转目标预扫描（正跳/回跳先建标签；落位在 emitSt 逐语句处）
        collectJumpTargets(file.sts);

        // 主程序（帧 0）
        locals = new HashMap<String, Integer>();
        localCount = 0;
        emitBlock(file.sts, false);
        w8(VtOp.TERM); w8(0);                          // 主程序隐式收工（收尾释放/自动关照走）
        int mainLocals = localCount;

        // 函数体（按名序，入口 = 当前偏移）
        List<VtBc.Fn> fns = new ArrayList<VtBc.Fn>();
        for (int i = 0; i < fnOrder.size(); i++) {
            String fname = fnOrder.get(i);
            VtAst.FnSt f = fnAst.get(fname);
            locals = new HashMap<String, Integer>();
            localCount = 0;
            loopStack.clear();
            for (int j = 0; j < f.params.size(); j++) locals.put(f.params.get(j), Integer.valueOf(j));
            localCount = f.params.size();
            VtBc.Fn fn = new VtBc.Fn();
            fn.nargs = f.params.size();
            fn.entry = p;
            emitBlock(f.body.sts, true);
            w8(VtOp.RETE);                             // 隐式无值返回（结果未写）
            fn.locals = localCount;
            fns.add(fn);
        }
        for (Label l : jmpLabels.values()) if (l.off < 0) throw new IllegalStateException("跳转目标未落位");

        // 组装
        VtBc.Blob b = new VtBc.Blob();
        b.srcHash = srcHash;
        b.code = new byte[p];
        System.arraycopy(buf, 0, b.code, 0, p);
        b.consts = consts;
        b.slotCount = slotBase;
        b.mainLocals = mainLocals;
        for (int i = 0; i < ts.size(); i++) b.tables.add(sym.tables.get(ts.get(i)));
        b.fns = fns;
        b.lines = lines;
        for (int i = 0; i < gs.size(); i++) addName(0, globals.get(gs.get(i)).intValue(), gs.get(i));
        for (int i = 0; i < ts.size(); i++) addName(1, tables.get(ts.get(i)).intValue(), ts.get(i));
        for (int i = 0; i < fs.size(); i++) addName(2, fnIdx.get(fs.get(i)).intValue(), fs.get(i));
        b.names = names;
        return b;
    }

    private void addName(int kind, int index, String name) {
        VtBc.Name n = new VtBc.Name();
        n.kind = kind; n.index = index; n.nameConst = constStr(name);
        names.add(n);
    }

    private void recLine(int line) {
        if (lastLine == line) return;
        lastLine = line;
        VtBc.Line l = new VtBc.Line();
        l.off = p; l.line = line;
        lines.add(l);
    }

    /* ---------- 名字解析 ---------- */
    private int slotOf(String name) {
        Integer li = locals.get(name);
        if (li != null) return slotBase + li.intValue();
        Integer gi = globals.get(name);
        if (gi == null) throw new IllegalStateException("名字缺槽: " + name);
        return gi.intValue();
    }
    private int roundSlot() {
        if (loopStack.isEmpty()) throw new IllegalStateException("循环外引用 轮");
        return slotBase + loopStack.get(loopStack.size() - 1).round;
    }
    private int allocLocal() { return slotBase + (localCount++); }

    /* ---------- 语句 ---------- */
    /** 预扫描顶层 跳转 目标行 → 预建标签（回跳/自跳也保证能在目标语句处落位；
     *  checker 已确保目标 ∈ 顶层语句行且非自身）。 */
    private void collectJumpTargets(List<VtAst.St> sts) {
        for (int i = 0; i < sts.size(); i++) {
            if (sts.get(i) instanceof VtAst.JmpSt) {
                VtAst.JmpSt j = (VtAst.JmpSt) sts.get(i);
                if (!j.end && !jmpLabels.containsKey(Integer.valueOf(j.target))) {
                    jmpLabels.put(Integer.valueOf(j.target), new Label());
                }
            }
        }
    }

    private void emitBlock(List<VtAst.St> sts, boolean inFn) {
        for (int i = 0; i < sts.size(); i++) emitSt(sts.get(i), inFn);
    }

    private void emitSt(VtAst.St s, boolean inFn) {
        if (s instanceof VtAst.FnSt || s instanceof VtAst.TableSt) return;   // 声明已在容器
        if (!inFn) {
            Label jl = jmpLabels.get(Integer.valueOf(s.line));
            if (jl != null && jl.off < 0) place(jl);   // 只落一次（同行多语句不重锚）
        }
        recLine(s.line);

        if (s instanceof VtAst.ActionSt) {
            VtAst.ActionSt a = (VtAst.ActionSt) s;
            if (a.kw == VtTok.K_DIANAN) {
                cex(a.args.get(0)); cex(a.args.get(1));
                if (a.args.size() > 2) cex(a.args.get(2)); else pushNum(50);
                w8(VtOp.TAP);
            } else if (a.kw == VtTok.K_HUADONG) {
                cex(a.args.get(0)); cex(a.args.get(1)); cex(a.args.get(2)); cex(a.args.get(3));
                cexOpt(a.opts, "时长", 300); cexOpt(a.opts, "停1", 0); cexOpt(a.opts, "停2", 0);
                w8(VtOp.SWIPE);
            } else if (a.kw == VtTok.K_DENGDAI) {
                cex(a.args.get(0)); w8(VtOp.WAIT);
            } else if (a.kw == VtTok.K_ANXIA) {
                cex(a.args.get(0)); cex(a.args.get(1)); w8(VtOp.DOWN);
            } else {
                w8(VtOp.UP);
            }
            return;
        }
        if (s instanceof VtAst.AssignSt) {
            VtAst.AssignSt a = (VtAst.AssignSt) s;
            if (a.idx == null) {
                cex(a.val);
                w8(VtOp.SV); w32v(slotOf(a.name));
            } else {
                cex(a.idx); cex(a.val);
                w8(VtOp.ST); w32v(tables.get(a.name).intValue());
            }
            return;
        }
        if (s instanceof VtAst.IfSt) {
            VtAst.IfSt f = (VtAst.IfSt) s;
            Label lelse = new Label(), lend = new Label();
            cex(f.cond);
            jf(lelse);
            emitBlock(f.thenB.sts, inFn);
            if (f.elseB != null) {
                jmp(lend);
                place(lelse);
                emitBlock(f.elseB.sts, inFn);
                place(lend);
            } else {
                place(lelse);
            }
            return;
        }
        if (s instanceof VtAst.LoopSt) {
            emitLoop((VtAst.LoopSt) s, inFn);
            return;
        }
        if (s instanceof VtAst.WhileSt) {
            VtAst.WhileSt w = (VtAst.WhileSt) s;
            int rslot = allocLocal();
            pushNum(0); w8(VtOp.SV); w32v(rslot);
            Loop lc = new Loop(); lc.round = localCount - 1;
            Label top = new Label();
            place(top);
            cex(w.cond);
            if (w.neg) jt(lc.brk); else jf(lc.brk);
            loopStack.add(lc);
            emitBlock(w.body.sts, inFn);
            loopStack.remove(loopStack.size() - 1);
            place(lc.cont);
            bumpRound(rslot);
            jmp(top);
            place(lc.brk);
            return;
        }
        if (s instanceof VtAst.CtlSt) {
            VtAst.CtlSt c = (VtAst.CtlSt) s;
            if (loopStack.size() < c.layers) throw new IllegalStateException("跳出层数");
            Loop lc = loopStack.get(loopStack.size() - c.layers);
            if (c.kw == VtTok.K_TIAOCHU) jmp(lc.brk); else jmp(lc.cont);
            return;
        }
        if (s instanceof VtAst.TermSt) {
            w8(VtOp.TERM);
            w8(((VtAst.TermSt) s).abort ? 1 : 0);
            return;
        }
        if (s instanceof VtAst.RetSt) {
            VtAst.RetSt r = (VtAst.RetSt) s;
            if (r.val != null) { cex(r.val); w8(VtOp.RET); }
            else w8(VtOp.RETE);
            return;
        }
        if (s instanceof VtAst.ToastSt) {
            VtAst.ToastSt t = (VtAst.ToastSt) s;
            cex(t.msg);
            cexOpt(t.opts, "时长", 2000);
            w8(VtOp.TOAST);
            return;
        }
        if (s instanceof VtAst.HlSt) {
            VtAst.HlSt h = (VtAst.HlSt) s;
            cexOpt(h.opts, "时长", 3000);
            w8(VtOp.HL); w32v(constStr(h.region));
            return;
        }
        if (s instanceof VtAst.BoxSt) {
            VtAst.BoxSt x = (VtAst.BoxSt) s;
            cex(x.x1); cex(x.y1); cex(x.x2); cex(x.y2);
            cexOpt(x.opts, "时长", 3000);
            w8(VtOp.BOX);
            return;
        }
        if (s instanceof VtAst.CallSt) {
            cex(((VtAst.CallSt) s).call);
            w8(VtOp.POP);
            return;
        }
        if (s instanceof VtAst.JmpSt) {
            VtAst.JmpSt j = (VtAst.JmpSt) s;
            if (j.end) { w8(VtOp.TERM); w8(0); return; }
            Label l = jmpLabels.get(Integer.valueOf(j.target));
            if (l == null) { l = new Label(); jmpLabels.put(Integer.valueOf(j.target), l); }
            cjmp(l);
            return;
        }
    }

    private void emitLoop(VtAst.LoopSt l, boolean inFn) {
        if (l.mode == 0 || (l.mode == 2 && hasKey(l.opts, "次数"))) {
            VtAst.Ex cnt = (l.mode == 0) ? l.count : optVal(l.opts, "次数");
            int cabs = allocLocal();
            int rslotAbs = allocLocal();
            Loop lc = new Loop(); lc.round = localCount - 1;
            Label top = new Label();
            cex(cnt);
            w8(VtOp.FLOOR); w8(VtOp.SV); w32v(cabs);
            pushNum(0); w8(VtOp.SV); w32v(rslotAbs);
            place(top);
            w8(VtOp.LV); w32v(cabs); pushNum(0);
            w8(VtOp.CMP); w8(VtOp.C_GT);
            jf(lc.brk);
            loopStack.add(lc);
            emitBlock(l.body.sts, inFn);
            loopStack.remove(loopStack.size() - 1);
            place(lc.cont);
            w8(VtOp.LV); w32v(cabs); pushNum(1); w8(VtOp.SUB); w8(VtOp.SV); w32v(cabs);
            bumpRound(rslotAbs);
            jmp(top);
            place(lc.brk);
            return;
        }
        if (l.mode == 2 && hasKey(l.opts, "时长")) {
            int dabs = allocLocal();
            int tabs = allocLocal();
            int rslotAbs = allocLocal();
            Loop lc = new Loop(); lc.round = localCount - 1;
            Label top = new Label();
            cex(optVal(l.opts, "时长")); w8(VtOp.SV); w32v(dabs);
            w8(VtOp.NOW); w8(VtOp.SV); w32v(tabs);
            pushNum(0); w8(VtOp.SV); w32v(rslotAbs);
            place(top);
            w8(VtOp.NOW); w8(VtOp.LV); w32v(tabs); w8(VtOp.SUB);
            w8(VtOp.LV); w32v(dabs);
            w8(VtOp.CMP); w8(VtOp.C_LT);
            jf(lc.brk);
            loopStack.add(lc);
            emitBlock(l.body.sts, inFn);
            loopStack.remove(loopStack.size() - 1);
            place(lc.cont);
            bumpRound(rslotAbs);
            jmp(top);
            place(lc.brk);
            return;
        }
        if (l.mode == 1) {
            int rslotAbs = allocLocal();
            Loop lc = new Loop(); lc.round = localCount - 1;
            Label top = new Label();
            pushNum(0); w8(VtOp.SV); w32v(rslotAbs);
            place(top);
            loopStack.add(lc);
            emitBlock(l.body.sts, inFn);
            loopStack.remove(loopStack.size() - 1);
            place(lc.cont);
            bumpRound(rslotAbs);
            jmp(top);
            place(lc.brk);
            return;
        }
        // for（含端点；正常跑完计数变量归位 = 上限值）
        String var = l.var;
        Integer prevBind = locals.get(var);
        int vabs = slotBase + (localCount++);
        locals.put(var, Integer.valueOf(vabs - slotBase));
        int tlab = allocLocal();
        int rslotAbs = allocLocal();
        Loop lc = new Loop(); lc.round = localCount - 1;
        Label top = new Label(), norm = new Label();
        cex(l.from); w8(VtOp.SV); w32v(vabs);
        cex(l.to); w8(VtOp.SV); w32v(tlab);
        pushNum(0); w8(VtOp.SV); w32v(rslotAbs);
        place(top);
        w8(VtOp.LV); w32v(vabs); w8(VtOp.LV); w32v(tlab);
        w8(VtOp.CMP); w8(VtOp.C_LE);
        jf(norm);
        loopStack.add(lc);
        emitBlock(l.body.sts, inFn);
        loopStack.remove(loopStack.size() - 1);
        place(lc.cont);
        w8(VtOp.LV); w32v(vabs); pushNum(1); w8(VtOp.ADD); w8(VtOp.SV); w32v(vabs);
        bumpRound(rslotAbs);
        jmp(top);
        place(norm);
        w8(VtOp.LV); w32v(tlab); w8(VtOp.SV); w32v(vabs);
        place(lc.brk);
        if (prevBind != null) locals.put(var, prevBind);   // 遮蔽还原；否则绑定持续（帧局部）
    }

    private void bumpRound(int rslotAbs) {
        w8(VtOp.LV); w32v(rslotAbs); pushNum(1); w8(VtOp.ADD); w8(VtOp.SV); w32v(rslotAbs);
    }

    /* ---------- 表达式 ---------- */
    private void cex(VtAst.Ex e) {
        if (e instanceof VtAst.NumEx) { pushNum(((VtAst.NumEx) e).v); return; }
        if (e instanceof VtAst.StrEx) { cexStr((VtAst.StrEx) e); return; }
        if (e instanceof VtAst.LitEx) {
            int kw = ((VtAst.LitEx) e).kw;
            if (kw == VtTok.K_LUN) { w8(VtOp.LV); w32v(roundSlot()); }
            else pushNum(kw == VtTok.K_ZHEN ? 1 : 0);
            return;
        }
        if (e instanceof VtAst.IdEx) { w8(VtOp.LV); w32v(slotOf(((VtAst.IdEx) e).name)); return; }
        if (e instanceof VtAst.IdxEx) {
            VtAst.IdxEx x = (VtAst.IdxEx) e;
            cex(x.idx);
            w8(VtOp.LT); w32v(tables.get(x.name).intValue());
            return;
        }
        if (e instanceof VtAst.BinEx) { cexBin((VtAst.BinEx) e); return; }
        if (e instanceof VtAst.UnEx) {
            VtAst.UnEx u = (VtAst.UnEx) e;
            cex(u.e);
            w8(u.op.equals("-") ? VtOp.NEG : VtOp.NOT);
            return;
        }
        if (e instanceof VtAst.CallEx) { cexCall((VtAst.CallEx) e); return; }
        throw new IllegalStateException("表达式节点");
    }

    private void cexStr(VtAst.StrEx s) {
        if (s.segs.size() == 1 && (s.segs.get(0) instanceof String)) {
            w8(VtOp.KS); w32v(constStr((String) s.segs.get(0)));
            return;
        }
        boolean first = true;
        for (int i = 0; i < s.segs.size(); i++) {
            Object o = s.segs.get(i);
            if (o instanceof String) {
                w8(VtOp.KS); w32v(constStr((String) o));
            } else {
                VtTok.Interp ip = (VtTok.Interp) o;
                if (ip.name.equals("轮")) { w8(VtOp.LV); w32v(roundSlot()); }
                else if (ip.name.equals("真")) pushNum(1);
                else if (ip.name.equals("假")) pushNum(0);
                else if (ip.hasIdx) {
                    if (ip.idxNum) pushNum(ip.idx);
                    else if (ip.idxName.equals("轮")) { w8(VtOp.LV); w32v(roundSlot()); }
                    else { w8(VtOp.LV); w32v(slotOf(ip.idxName)); }
                    w8(VtOp.LT); w32v(tables.get(ip.name).intValue());
                } else {
                    w8(VtOp.LV); w32v(slotOf(ip.name));
                }
            }
            if (!first) w8(VtOp.ADD);
            first = false;
        }
        if (first) { w8(VtOp.KS); w32v(constStr("")); }
    }

    private void cexBin(VtAst.BinEx b) {
        if (b.op.equals("&&")) {
            Label lf = new Label(), le = new Label();
            cex(b.l); jf(lf);
            cex(b.r); jf(lf);
            pushNum(1); jmp(le);
            place(lf); pushNum(0);
            place(le);
            return;
        }
        if (b.op.equals("||")) {
            Label lt = new Label(), le = new Label();
            cex(b.l); jt(lt);
            cex(b.r); jt(lt);
            pushNum(0); jmp(le);
            place(lt); pushNum(1);
            place(le);
            return;
        }
        cex(b.l); cex(b.r);
        if (b.op.equals("+")) { w8(VtOp.ADD); return; }
        if (b.op.equals("-")) { w8(VtOp.SUB); return; }
        if (b.op.equals("*")) { w8(VtOp.MUL); return; }
        if (b.op.equals("/")) { w8(VtOp.DIV); return; }
        if (b.op.equals("%")) { w8(VtOp.MOD); return; }
        w8(VtOp.CMP);
        w8(cmpCode(b.op));
    }

    private static int cmpCode(String o) {
        if (o.equals("==")) return VtOp.C_EQ;
        if (o.equals("!=")) return VtOp.C_NE;
        if (o.equals("<")) return VtOp.C_LT;
        if (o.equals(">")) return VtOp.C_GT;
        if (o.equals("<=")) return VtOp.C_LE;
        if (o.equals(">=")) return VtOp.C_GE;
        throw new IllegalStateException("比较符");
    }

    private void cexCall(VtAst.CallEx c) {
        Integer fi = fnIdx.get(c.name);
        if (fi != null) {
            for (int i = 0; i < c.args.size(); i++) cex(c.args.get(i));
            w8(VtOp.CALL); w32v(fi.intValue()); w8(c.args.size());
            return;
        }
        if (c.name.equals("len")) { cex(c.args.get(0)); w8(VtOp.LEN); return; }
        if (c.name.equals("str")) { cex(c.args.get(0)); w8(VtOp.STRN); return; }
        if (c.name.equals("num")) { cex(c.args.get(0)); w8(VtOp.NUM); return; }
        if (c.name.equals("rand")) { cex(c.args.get(0)); cex(c.args.get(1)); w8(VtOp.RAND); return; }
        if (c.name.equals("floor")) { cex(c.args.get(0)); w8(VtOp.FLOOR); return; }
        if (c.name.equals("ceil")) { cex(c.args.get(0)); w8(VtOp.CEIL); return; }
        if (c.name.equals("round")) { cex(c.args.get(0)); w8(VtOp.ROUND); return; }
        if (c.name.equals("abs")) { cex(c.args.get(0)); w8(VtOp.ABS); return; }
        if (c.name.equals("min")) { cex(c.args.get(0)); cex(c.args.get(1)); w8(VtOp.MIN); return; }
        if (c.name.equals("max")) { cex(c.args.get(0)); cex(c.args.get(1)); w8(VtOp.MAX); return; }
        if (c.name.equals("sqrt")) { cex(c.args.get(0)); w8(VtOp.SQRT); return; }
        if (c.name.equals("sin")) { cex(c.args.get(0)); w8(VtOp.SIN); return; }
        if (c.name.equals("cos")) { cex(c.args.get(0)); w8(VtOp.COS); return; }
        if (c.name.equals("atan2")) { cex(c.args.get(0)); cex(c.args.get(1)); w8(VtOp.ATAN2); return; }
        if (c.name.equals("找图")) {
            String tpl = litOf(c.args.get(0), "模板");
            String reg = regionOf(c.opts);
            cexOpt(c.opts, "阈值", 8); cexOpt(c.opts, "色校", 0); cexOpt(c.opts, "持续", 0);
            w8(VtOp.FINDIMG); w32v(constStr(tpl)); w32v(reg == null ? -1 : constStr(reg));
            return;
        }
        if (c.name.equals("找色")) {
            String set = litOf(c.args.get(0), "点集");
            String reg = regionOf(c.opts);
            w8(VtOp.FINDCOLOR); w32v(constStr(set)); w32v(reg == null ? -1 : constStr(reg));
            return;
        }
        if (c.name.equals("找色点")) {
            String reg = regionOf(c.opts);
            cex(c.args.get(0));
            cexOpt(c.opts, "容差", 8);
            w8(VtOp.FINDDOT); w32v(reg == null ? -1 : constStr(reg));
            return;
        }
        if (c.name.equals("在区域")) {
            String reg = litOf(c.args.get(0), "区域");
            cex(c.args.get(1)); cex(c.args.get(2));
            w8(VtOp.INREGION); w32v(constStr(reg));
            return;
        }
        if (c.name.equals("开关开")) {
            String nm = litOf(c.args.get(0), "开关名");
            w8(VtOp.SWON); w32v(constStr(nm));
            return;
        }
        throw new IllegalStateException("调用 " + c.name);
    }

    private String litOf(VtAst.Ex e, String what) {
        if (e instanceof VtAst.StrEx) {
            VtAst.StrEx s = (VtAst.StrEx) e;
            if (s.segs.size() == 1 && (s.segs.get(0) instanceof String)) return (String) s.segs.get(0);
        }
        throw new IllegalStateException("需字符串字面量: " + what);
    }

    private String regionOf(List<VtAst.Opt> opts) {
        for (int i = 0; i < opts.size(); i++) {
            VtAst.Opt o = opts.get(i);
            if (o.key.equals("区域")) return litOf(o.val, "区域");
        }
        return null;
    }

    /* ---------- 选项取用 ---------- */
    private static boolean hasKey(List<VtAst.Opt> opts, String key) {
        for (int i = 0; i < opts.size(); i++) if (opts.get(i).key.equals(key)) return true;
        return false;
    }
    private static VtAst.Ex optVal(List<VtAst.Opt> opts, String key) {
        for (int i = 0; i < opts.size(); i++) if (opts.get(i).key.equals(key)) return opts.get(i).val;
        return null;
    }
    private void cexOpt(List<VtAst.Opt> opts, String key, double def) {
        VtAst.Ex v = optVal(opts, key);
        if (v != null) cex(v); else pushNum(def);
    }
}
