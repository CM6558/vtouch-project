package com.vtouch.editor;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * vt-script → 步骤表投影器（V8 T6.1「AST → 块渲染」的算核）：脚本源码 → Ops.Step[]（可视化子集）。
 *
 * 与 VtFromSteps（steps→script，T5.1）互为逆：往返判据 = translate(project(src)) 与再跑一遍全链的输出
 * 逐字一致（固定点自检，project() 内置——不稳定的投影一律拒绝，宁缺毋滥）。
 * 投影保证（对 VtFromSteps 全部产物成立）：translate(project(src)) == 规范打印(src)，语料级逐字核过
 * （VtToStepsCheck / 设备测试台双测）。
 *
 * 子集边界（T6 v1「覆盖清单」；超出即拒绝并给「第 N 行: …」原因）：
 *  - 语句：点按/滑动/等待/按下/弹起/赋值（v5 表达式域）/如果·否则（判定原子组合）/循环×4（次数/时长/无限/直到）
 *    /跳出·继续（层数）/终止/提示/框区域/框矩形 —— 全部覆盖；
 *  - 仅脚本（拒绝切块）：跳转（兼容语法，SPEC §8）、函数/表/返回/调用语句、当循环、for 计数循环；
 *  - 字段原子：整数≥0 字面量 或 单个变量引用（预置 7 或自定义槽）；负数字面量/小数/复合表达式 → 拒绝
 *    （负值在步骤字段里 = 变量编码，语义冲突）。
 *  - 规范重排（可接受、有固定点保障）：缺省参数补显（点按 ms=50、滑动时长=300、提示 2000…）、
 *    显式默认值折叠（找图 阈值=8 / 容差=8 / 停1=0…）、组合条件的德摩根 DNF 展开。
 *
 * 纯 Java、零 Android 依赖（离机核 VtToStepsCheck；设备测试台「T6 双视图」核）。
 */
public final class VtToSteps {

    /** 投影结果。err == null → op 可用（op.name/gate/autooff 由调用方填）。 */
    public static final class R {
        public Ops.Op op;                       // steps 已填
        public String err;                      // 第一个原因（已带「第 N 行: 」）
        public String src;                      // 投影后再翻译产出的规范脚本（= translate(op)）
        public boolean exact;                   // 原文是否本就规范（src == 规范打印(原文)）
        public final List<String> reasons = new ArrayList<String>();   // 全部拒绝原因（≤20 条）
    }

    // ---------------- 入口 ----------------

    /** 全链投影：注释扫描 → 解析 → 静态检查 → 投影 → 固定点自检。 */
    public static R project(String src) {
        R r = new R();
        String s = src == null ? "" : src;
        int cm = userCommentLine(s);
        if (cm > 0) {
            r.err = "第 " + cm + " 行: 含注释（块视图不保留注释；请删注释或留在脚本模式）";
            r.reasons.add(r.err);
            return r;
        }
        VtParser.R pp = VtParser.parse(s);
        if (pp.err != null) { r.err = "第 " + pp.line + " 行: " + pp.err; r.reasons.add(r.err); return r; }
        VtCheck.R ck = VtCheck.run(s);
        if (ck.err != null) { r.err = "第 " + ck.line + " 行: " + ck.err; r.reasons.add(r.err); return r; }
        R r1 = raw(pp.file);
        if (r1.err != null) { return r1; }
        VtFromSteps.R t1 = VtFromSteps.translate(r1.op);
        if (t1.src == null) {
            r.err = "投影自检失败：" + (t1.problems.isEmpty() ? "（未知）" : t1.problems.get(0));
            r.reasons.add(r.err);
            return r;
        }
        VtParser.R p2 = VtParser.parse(t1.src);
        if (p2.err != null) { r.err = "投影自检失败（再解析）：" + p2.err; r.reasons.add(r.err); return r; }
        R r2 = raw(p2.file);
        if (r2.err != null) { r.err = "投影自检失败（重投影）：" + r2.err; r.reasons.add(r.err); return r; }
        VtFromSteps.R t2 = VtFromSteps.translate(r2.op);
        if (t2.src == null || !t2.src.equals(t1.src)) {
            r.err = "投影自检不一致（固定点失败）";
            r.reasons.add(r.err);
            return r;
        }
        String canon = VtPrint.print(pp.file);
        r.op = r1.op;
        r.src = t1.src;
        r.exact = canon.equals(t1.src);
        return r;
    }

    /** 用户注释扫描：字符串内 `#` 不算；`#模式 块/脚本` 侧线（编辑器模式锁）不算。返回首个注释行号（0 = 无）。 */
    static int userCommentLine(String src) {
        boolean inStr = false;
        int line = 1;
        for (int i = 0; i < src.length(); i++) {
            char c = src.charAt(i);
            if (c == '\n') line++;
            if (inStr) {
                if (c == '\\') { i++; continue; }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '#') {
                int e = src.indexOf('\n', i);
                if (e < 0) e = src.length();
                String rest = src.substring(i, e).replaceAll("\\s+", "");
                if (!(rest.equals("#模式块") || rest.equals("#模式脚本"))) return line;
                i = e - 1;
            }
        }
        return 0;
    }

    /** 投影本体（无固定点自检；自检用二跑路径）。 */
    private static R raw(VtAst.Block file) {
        VtToSteps p = new VtToSteps();
        p.r = new R();
        p.r.op = new Ops.Op();
        p.collectVars(file);
        if (p.r.reasons.isEmpty()) p.walk(file.sts, 0);
        if (!p.r.reasons.isEmpty()) {
            p.r.err = p.r.reasons.get(0);
            p.r.op = null;
        }
        return p.r;
    }

    // ---------------- 内部状态 ----------------

    private R r;
    private final Map<String, Integer> slots = new HashMap<String, Integer>();   // 变量名 → 槽 0..15
    private final List<String> slotNames = new ArrayList<String>();
    private static final String[] PRE = VtCheck.PRE_VARS;
    private int stepCount;

    private static final int BAD = Integer.MIN_VALUE;

    private void reason(int line, String why) {
        if (r.reasons.size() < 20) r.reasons.add("第 " + line + " 行: " + why);
    }

    private static boolean isPre(String n) {
        for (String p : PRE) if (p.equals(n)) return true;
        return false;
    }

    /** 变量引用名 → 步骤编码（预置或槽）；无效返回 BAD。 */
    private int varCode(String name) {
        for (int i = 0; i < PRE.length; i++) if (PRE[i].equals(name)) return -(i + 1);   // -1..-7
        Integer k = slots.get(name);
        if (k != null) return Ops.customCode(k.intValue());
        return BAD;
    }

    // ---------------- pass 1：变量收集（首次赋值序） ----------------

    private void collectVars(VtAst.Block f) {
        collectVars(f.sts);
    }

    private void collectVars(List<VtAst.St> sts) {
        for (VtAst.St s : sts) {
            if (s instanceof VtAst.AssignSt) {
                VtAst.AssignSt a = (VtAst.AssignSt) s;
                if (a.idx != null) { reason(s.line, "表读写暂不支持块视图（" + a.name + "[…]）"); continue; }
                if (isPre(a.name)) { reason(s.line, "预置变量不可作赋值目标（" + a.name + "）——块视图不支持"); continue; }
                if (slots.containsKey(a.name)) continue;
                if (!Regions.varOk(a.name)) { reason(s.line, "变量名超长或非法（块模式上限 15 字）：" + a.name); continue; }
                if (slots.size() >= Ops.VAR_N) { reason(s.line, "变量数超过 " + Ops.VAR_N + "（块视图上限）：" + a.name); continue; }
                slots.put(a.name, Integer.valueOf(slots.size()));
                slotNames.add(a.name);
            } else if (s instanceof VtAst.IfSt) {
                collectVars(((VtAst.IfSt) s).thenB.sts);
                if (((VtAst.IfSt) s).elseB != null) collectVars(((VtAst.IfSt) s).elseB.sts);
            } else if (s instanceof VtAst.LoopSt) {
                collectVars(((VtAst.LoopSt) s).body.sts);
            } else if (s instanceof VtAst.WhileSt) {
                collectVars(((VtAst.WhileSt) s).body.sts);
            }
        }
    }

    private String[] varSlotsArr() {
        return slotNames.toArray(new String[0]);
    }

    // ---------------- pass 2：语句遍历 ----------------

    private void walk(List<VtAst.St> sts, int depth) {
        for (VtAst.St s : sts) {
            if (!r.reasons.isEmpty()) return;
            walkOne(s, depth);
        }
    }

    private void walkOne(VtAst.St s, int depth) {
        if (s instanceof VtAst.ActionSt) {
            action((VtAst.ActionSt) s);
        } else if (s instanceof VtAst.AssignSt) {
            assign((VtAst.AssignSt) s);
        } else if (s instanceof VtAst.IfSt) {
            ifSt((VtAst.IfSt) s, depth);
        } else if (s instanceof VtAst.LoopSt) {
            loopSt((VtAst.LoopSt) s, depth);
        } else if (s instanceof VtAst.WhileSt) {
            whileSt((VtAst.WhileSt) s, depth);
        } else if (s instanceof VtAst.CtlSt) {
            VtAst.CtlSt c = (VtAst.CtlSt) s;
            Ops.Step st = new Ops.Step();
            st.type = (c.kw == VtTok.K_TIAOCHU) ? Ops.T_BREAK : Ops.T_CONT;
            st.a1 = c.layers;
            add(st);
        } else if (s instanceof VtAst.TermSt) {
            Ops.Step st = new Ops.Step();
            st.type = Ops.T_STOP;
            st.a1 = ((VtAst.TermSt) s).abort ? 1 : 0;
            add(st);
        } else if (s instanceof VtAst.ToastSt) {
            toast((VtAst.ToastSt) s);
        } else if (s instanceof VtAst.HlSt) {
            hl((VtAst.HlSt) s);
        } else if (s instanceof VtAst.BoxSt) {
            box((VtAst.BoxSt) s);
        } else if (s instanceof VtAst.JmpSt) {
            reason(s.line, "含兼容语法「跳转」，仅脚本模式（SPEC §8）");
        } else if (s instanceof VtAst.FnSt) {
            reason(s.line, "函数定义暂不支持块视图");
        } else if (s instanceof VtAst.TableSt) {
            reason(s.line, "表定义暂不支持块视图");
        } else if (s instanceof VtAst.RetSt) {
            reason(s.line, "返回暂不支持块视图");
        } else if (s instanceof VtAst.CallSt) {
            reason(s.line, "语句位调用暂不支持块视图");
        } else {
            reason(s.line, "语句暂不支持块视图");
        }
    }

    private void add(Ops.Step st) {
        stepCount++;
        if (stepCount > Ops.MAX_STEPS) { reason(1, "步骤数超过 " + Ops.MAX_STEPS); return; }
        r.op.steps.add(st);
    }

    private void action(VtAst.ActionSt a) {
        Ops.Step st = new Ops.Step();
        switch (a.kw) {
            case VtTok.K_DIANAN: {
                st.type = Ops.T_TAP;
                st.a1 = atomInt(a.args.get(0), false, a.line);
                st.a2 = atomInt(a.args.get(1), false, a.line);
                if (a.args.size() >= 3) st.ms = atomInt(a.args.get(2), false, a.line);
                else st.ms = 50;                                   // 规范默认（SPEC §3）
                noOpts(a.opts, new String[]{}, a.line);
                break;
            }
            case VtTok.K_HUADONG: {
                st.type = Ops.T_SWIPE;
                st.a1 = atomInt(a.args.get(0), false, a.line);
                st.a2 = atomInt(a.args.get(1), false, a.line);
                st.a3 = atomInt(a.args.get(2), false, a.line);
                st.a4 = atomInt(a.args.get(3), false, a.line);
                st.ms = 300;                                       // [对齐] 默认时长 300
                for (VtAst.Opt o : a.opts) {
                    if (o.key.equals("时长")) st.ms = atomInt(o.val, false, o.line);
                    else if (o.key.equals("停1")) st.j1 = atomInt(o.val, false, o.line);
                    else if (o.key.equals("停2")) st.j2 = atomInt(o.val, false, o.line);
                    else reason(o.line, "滑动不支持参数 " + o.key);
                }
                break;
            }
            case VtTok.K_DENGDAI: {
                st.type = Ops.T_WAIT;
                st.ms = atomInt(a.args.get(0), false, a.line);
                noOpts(a.opts, new String[]{}, a.line);
                break;
            }
            case VtTok.K_ANXIA: {
                st.type = Ops.T_DOWN;
                st.a1 = atomInt(a.args.get(0), false, a.line);
                st.a2 = atomInt(a.args.get(1), false, a.line);
                noOpts(a.opts, new String[]{}, a.line);
                break;
            }
            case VtTok.K_TANQI: {
                st.type = Ops.T_UP;
                noOpts(a.opts, new String[]{}, a.line);
                break;
            }
            default:
                reason(a.line, "动作暂不支持块视图");
                return;
        }
        if (st.a1 == BAD || st.a2 == BAD || st.a3 == BAD || st.a4 == BAD || st.ms == BAD) return;
        add(st);
    }

    private void noOpts(List<VtAst.Opt> opts, String[] keys, int line) {
        if (opts == null) return;
        for (VtAst.Opt o : opts) {
            boolean ok = false;
            for (String k : keys) if (k.equals(o.key)) ok = true;
            if (!ok) reason(o.line, "该语句不支持参数 " + o.key);
        }
    }

    private void assign(VtAst.AssignSt a) {
        Integer k = slots.get(a.name);
        if (k == null) {                                   // 已在 pass1 记因（预置/超限等）
            if (isPre(a.name)) reason(a.line, "预置变量不可作赋值目标（" + a.name + "）");
            else if (!Regions.varOk(a.name)) { /* pass1 已记 */ }
            else reason(a.line, "变量未登记（" + a.name + "）");
            return;
        }
        Ops.Step st = new Ops.Step();
        st.type = Ops.T_CALC;
        st.a1 = k.intValue();
        st.ref = a.name;
        st.expr = VtPrint.printExpr(a.val);
        String why = Expr.check(st.expr, java.util.Arrays.asList(varSlotsArr()));
        if (why != null) {
            reason(a.line, "表达式超出块视图支持（" + why + "）");
            return;
        }
        add(st);
    }

    private void ifSt(VtAst.IfSt f, int depth) {
        if (depth >= 8) { reason(f.line, "嵌套超过 8 层（块视图上限）"); return; }
        List<Atom> atoms = dnf(f.cond, false, f.line);
        if (atoms == null) return;
        if (atoms.size() > Ops.CLAUSE_MAX) { reason(f.line, "组合条件超过 " + Ops.CLAUSE_MAX + " 条（块视图上限）"); return; }
        // 头 + 子条件
        Ops.Step head = atomStep(Ops.T_IF, atoms.get(0));
        add(head);
        for (int i = 1; i < atoms.size(); i++) {
            Atom at = atoms.get(i);
            Ops.Step cl = atomStep(Ops.T_CLAUSE, at);
            cl.a1 = at.newGroup ? Ops.J_OR : Ops.J_AND;
            add(cl);
        }
        if (!r.reasons.isEmpty()) return;
        walk(f.thenB.sts, depth + 1);
        if (f.elseB != null) {
            Ops.Step el = new Ops.Step();
            el.type = Ops.T_ELSE;
            add(el);
            walk(f.elseB.sts, depth + 1);
        }
        Ops.Step end = new Ops.Step();
        end.type = Ops.T_ENDIF;
        add(end);
    }

    private void loopSt(VtAst.LoopSt l, int depth) {
        if (depth >= 8) { reason(l.line, "嵌套超过 8 层（块视图上限）"); return; }
        if (l.mode == 3) { reason(l.line, "「循环 i 从 a 到 b」暂不支持块视图（可改写为「循环 N」）"); return; }
        Ops.Step head = new Ops.Step();
        head.type = Ops.T_LOOP;
        if (l.mode == 1) {                                  // 无限
            head.a1 = 2;
        } else if (l.mode == 0) {                           // 计数表达式
            head.a1 = 0;
            head.a2 = litInt(l.count, false, l.line);
        } else {                                            // 选项键：次数= / 时长=
            VtAst.Opt cnt = optOf(l.opts, "次数"), dur = optOf(l.opts, "时长");
            if (cnt != null && dur != null) { reason(l.line, "循环不能同时写 次数= 与 时长="); return; }
            if (cnt != null) { head.a1 = 0; head.a2 = litInt(cnt.val, false, cnt.line); }
            else if (dur != null) { head.a1 = 1; head.a2 = litInt(dur.val, false, dur.line); }
            else { reason(l.line, "循环头缺 次数= / 时长="); return; }
        }
        if (head.a2 == BAD) return;
        add(head);
        walk(l.body.sts, depth + 1);
        Ops.Step end = new Ops.Step();
        end.type = Ops.T_ENDLOOP;
        add(end);
    }

    private void whileSt(VtAst.WhileSt w, int depth) {
        if (depth >= 8) { reason(w.line, "嵌套超过 8 层（块视图上限）"); return; }
        if (!w.neg) {
            reason(w.line, "「当」循环暂不支持块视图（可改写为「直到 !(条件)」）");
            return;
        }
        List<Atom> atoms = dnf(w.cond, false, w.line);
        if (atoms == null) return;
        if (atoms.size() > Ops.CLAUSE_MAX) { reason(w.line, "组合条件超过 " + Ops.CLAUSE_MAX + " 条（块视图上限）"); return; }
        Ops.Step head = atomStep(Ops.T_LOOP, atoms.get(0));
        head.a1 = 3;                                        // 到条件
        head.a2 = atoms.get(0).jt; head.a3 = atoms.get(0).p1; head.a4 = atoms.get(0).p2;
        head.j2 = atoms.get(0).inv;
        head.ms = atoms.get(0).kms; head.j1 = atoms.get(0).pack;
        head.ref = atoms.get(0).ref; head.expr = atoms.get(0).region;
        add(head);
        for (int i = 1; i < atoms.size(); i++) {
            Ops.Step cl = atomStep(Ops.T_CLAUSE, atoms.get(i));
            cl.a1 = atoms.get(i).newGroup ? Ops.J_OR : Ops.J_AND;
            add(cl);
        }
        walk(w.body.sts, depth + 1);
        Ops.Step end = new Ops.Step();
        end.type = Ops.T_ENDLOOP;
        add(end);
    }

    private void toast(VtAst.ToastSt t) {
        if (!(t.msg instanceof VtAst.StrEx)) {
            reason(t.line, "提示的文案须为字符串字面量（块视图）");
            return;
        }
        VtAst.StrEx se = (VtAst.StrEx) t.msg;
        StringBuilder b = new StringBuilder();
        for (Object seg : se.segs) {
            if (seg instanceof String) {
                String x = (String) seg;
                for (int i = 0; i < x.length(); i++) {
                    char c = x.charAt(i);
                    if (c == '{') b.append("{{");
                    else if (c == '}') b.append("}}");
                    else b.append(c);
                }
            } else {
                VtTok.Interp ip = (VtTok.Interp) seg;
                if (ip.hasIdx) { reason(ip.line, "提示插值不支持表下标（块视图）"); return; }
                if (!isPre(ip.name) && !slots.containsKey(ip.name)) {
                    reason(ip.line, "提示占位变量无效（" + ip.name + "）");
                    return;
                }
                b.append('{').append(ip.name).append('}');
            }
        }
        String tpl = b.toString();
        String te = Ops.toastTplErr(tpl, java.util.Arrays.asList(varSlotsArr()));
        if (te != null) { reason(t.line, te + "（块视图）"); return; }
        Ops.Step st = new Ops.Step();
        st.type = Ops.T_TOAST;
        st.expr = tpl;
        st.ms = 0;
        for (VtAst.Opt o : t.opts) {
            if (o.key.equals("时长")) st.ms = litInt(o.val, false, o.line);
            else reason(o.line, "提示不支持参数 " + o.key);
        }
        if (st.ms == BAD) return;
        add(st);
    }

    private void hl(VtAst.HlSt h) {
        if (!Regions.idOk(h.region)) { reason(h.line, "框区域引用的区域名非法：" + h.region); return; }
        Ops.Step st = new Ops.Step();
        st.type = Ops.T_HL;
        st.ref = h.region;
        st.ms = 0;
        for (VtAst.Opt o : h.opts) {
            if (o.key.equals("时长")) st.ms = litInt(o.val, false, o.line);
            else reason(o.line, "框区域不支持参数 " + o.key);
        }
        if (st.ms == BAD) return;
        add(st);
    }

    private void box(VtAst.BoxSt x) {
        Ops.Step st = new Ops.Step();
        st.type = Ops.T_BOX;
        st.a1 = atomInt(x.x1, false, x.line);
        st.a2 = atomInt(x.y1, false, x.line);
        st.a3 = atomInt(x.x2, false, x.line);
        st.a4 = atomInt(x.y2, false, x.line);
        st.ms = 0;
        for (VtAst.Opt o : x.opts) {
            if (o.key.equals("时长")) st.ms = atomInt(o.val, false, o.line);
            else reason(o.line, "框矩形不支持参数 " + o.key);
        }
        if (st.a1 == BAD || st.a2 == BAD || st.a3 == BAD || st.a4 == BAD || st.ms == BAD) return;
        add(st);
    }

    private VtAst.Opt optOf(List<VtAst.Opt> opts, String key) {
        if (opts == null) return null;
        for (VtAst.Opt o : opts) if (o.key.equals(key)) return o;
        return null;
    }

    // ---------------- 字段原子 ----------------

    /** 字段整数：整数≥0 字面量 或 单变量引用（域与步骤世界 numOk/coordOk 一致）；否则记因返回 BAD。 */
    private int atomInt(VtAst.Ex e, boolean allowNeg, int fallLine) {
        if (e instanceof VtAst.IdEx) {
            int c = varCode(((VtAst.IdEx) e).name);
            if (c == BAD) reason(e.line > 0 ? e.line : fallLine, "变量无效：" + ((VtAst.IdEx) e).name);
            return c;
        }
        int v = litVal(e, allowNeg, fallLine);
        return v;
    }

    /** 字面量专用字段（循环次数/时长、提示/框区域时长、找图阈值·色校·持续、找色点容差、比较参照值）：
     *  步骤世界对这些字段是纯字面量域（validate/condErr 拒负值），变量一律拒。 */
    private int litInt(VtAst.Ex e, boolean allowNeg, int fallLine) {
        if (e instanceof VtAst.IdEx) {
            reason(e.line > 0 ? e.line : fallLine, "该字段不支持变量（块视图）：" + ((VtAst.IdEx) e).name);
            return BAD;
        }
        return litVal(e, allowNeg, fallLine);
    }

    private int litVal(VtAst.Ex e, boolean allowNeg, int fallLine) {
        if (e instanceof VtAst.UnEx && ((VtAst.UnEx) e).op.equals("-")
                && ((VtAst.UnEx) e).e instanceof VtAst.NumEx) {
            VtAst.NumEx n = (VtAst.NumEx) ((VtAst.UnEx) e).e;
            double v = n.v;
            int ln = e.line > 0 ? e.line : fallLine;
            if (!allowNeg) {
                reason(ln, "负数字面量无法进块字段（负值 = 变量编码）；负号留脚本模式");
                return BAD;
            }
            if (v == Math.floor(v) && v <= Integer.MAX_VALUE) return (int) -((long) v);
        }
        if (e instanceof VtAst.NumEx) {
            VtAst.NumEx n = (VtAst.NumEx) e;
            double v = n.v;
            if (v != Math.floor(v) || v > Integer.MAX_VALUE || v < Integer.MIN_VALUE) {
                reason(e.line > 0 ? e.line : fallLine, "小数超出块视图字段（只收整数）");
                return BAD;
            }
            long iv = (long) v;
            if (iv < 0 && !allowNeg) {
                reason(e.line > 0 ? e.line : fallLine, "负数字面量无法进块字段（负值 = 变量编码）；负号留脚本模式");
                return BAD;
            }
            return (int) iv;
        }
        reason(e.line > 0 ? e.line : fallLine, "字段超出块视图支持（须为整数/单个变量；复合表达式留在脚本模式）");
        return BAD;
    }

    // ---------------- 条件（DNF） ----------------

    /** DNF 原子：判定参数 + 取反 + 组边界（newGroup = 该原子开启新「或」组）。 */
    private static final class Atom {
        int jt, p1, p2, kms, pack, inv;
        String ref = "", region = "";
        boolean newGroup;
    }

    /** 条件表达式 → DNF 原子链（且=组内、或=组间）；null = 已记因。 */
    private List<Atom> dnf(VtAst.Ex e, boolean neg, int fallLine) {
        List<List<Atom>> groups = dnfRaw(e, neg);
        if (groups == null) return null;
        List<Atom> flat = new ArrayList<Atom>();
        for (int g = 0; g < groups.size(); g++) {
            List<Atom> grp = groups.get(g);
            for (int i = 0; i < grp.size(); i++) {
                Atom a = grp.get(i);
                a.newGroup = (i == 0 && g > 0);
                flat.add(a);
            }
        }
        return flat;
    }

    /** 德摩根展开：neg 穿透；返回「或组的列表，组内=且」。不可投影 → null（已记因）。 */
    private List<List<Atom>> dnfRaw(VtAst.Ex e, boolean neg) {
        if (e instanceof VtAst.UnEx && ((VtAst.UnEx) e).op.equals("!")) {
            return dnfRaw(((VtAst.UnEx) e).e, !neg);
        }
        if (e instanceof VtAst.BinEx) {
            VtAst.BinEx b = (VtAst.BinEx) e;
            if (b.op.equals("&&") || b.op.equals("||")) {
                boolean isOr = b.op.equals("||");
                List<List<Atom>> A = dnfRaw(b.l, neg);
                if (A == null) return null;
                List<List<Atom>> B = dnfRaw(b.r, neg);
                if (B == null) return null;
                boolean joinOr = isOr ^ neg;                    // 非负：|| → 或；取反后：&& → 或（德摩根）
                if (joinOr) {
                    List<List<Atom>> out = new ArrayList<List<Atom>>(A);
                    out.addAll(B);
                    return out;
                }
                List<List<Atom>> out = new ArrayList<List<Atom>>();
                for (List<Atom> ga : A) {
                    for (List<Atom> gb : B) {
                        List<Atom> g = new ArrayList<Atom>(ga);
                        g.addAll(gb);
                        out.add(g);
                    }
                }
                return out;
            }
        }
        Atom a = atom(e);
        if (a == null) return null;
        a.inv = neg ? 1 : 0;
        List<List<Atom>> out = new ArrayList<List<Atom>>();
        List<Atom> g = new ArrayList<Atom>();
        g.add(a);
        out.add(g);
        return out;
    }

    /** 判定原子：五类判定（在区域/开关开/找图/找色/找色点/变量比较）。 */
    private Atom atom(VtAst.Ex e) {
        if (e instanceof VtAst.CallEx) {
            VtAst.CallEx c = (VtAst.CallEx) e;
            Atom a = new Atom();
            if (c.name.equals("在区域")) {
                if (c.args.size() != 3 || !(c.args.get(0) instanceof VtAst.StrEx)) { reason(e.line, "在区域 参数不匹配（块视图）"); return null; }
                a.jt = 0;
                a.ref = strOf((VtAst.StrEx) c.args.get(0), e.line);
                if (a.ref == null) return null;
                a.p1 = atomInt(c.args.get(1), false, e.line);
                a.p2 = atomInt(c.args.get(2), false, e.line);
                if (a.p1 == BAD || a.p2 == BAD) return null;
                if (c.opts != null && !c.opts.isEmpty()) { reason(e.line, "在区域不支持参数（块视图）"); return null; }
                if (!Regions.idOk(a.ref)) { reason(e.line, "区域引用非法：" + a.ref); return null; }
                return a;
            }
            if (c.name.equals("开关开")) {
                if (c.args.size() != 1 || !(c.args.get(0) instanceof VtAst.StrEx)) { reason(e.line, "开关开 参数不匹配（块视图）"); return null; }
                a.jt = 1;
                a.ref = strOf((VtAst.StrEx) c.args.get(0), e.line);
                if (a.ref == null) return null;
                if (!Regions.idOk(a.ref)) { reason(e.line, "区域引用非法：" + a.ref); return null; }
                return a;
            }
            if (c.name.equals("找图")) {
                if (c.args.size() != 1 || !(c.args.get(0) instanceof VtAst.StrEx)) { reason(e.line, "找图 参数不匹配（块视图）"); return null; }
                a.jt = 2;
                a.ref = strOf((VtAst.StrEx) c.args.get(0), e.line);
                if (a.ref == null) return null;
                a.p1 = 8; a.p2 = 0; a.kms = 0;
                for (VtAst.Opt o : c.opts) {
                    if (o.key.equals("区域")) {
                        if (!(o.val instanceof VtAst.StrEx)) { reason(o.line, "找图 区域= 须为字符串字面量"); return null; }
                        a.region = strOf((VtAst.StrEx) o.val, o.line);
                        if (a.region == null) return null;
                    } else if (o.key.equals("阈值")) {
                        a.p1 = litInt(o.val, false, o.line);
                        if (a.p1 == BAD) return null;
                    } else if (o.key.equals("色校")) {
                        a.p2 = litInt(o.val, false, o.line);
                        if (a.p2 == BAD) return null;
                    } else if (o.key.equals("持续")) {
                        a.kms = litInt(o.val, false, o.line);
                        if (a.kms == BAD) return null;
                    } else { reason(o.line, "找图不支持参数 " + o.key); return null; }
                }
                if (a.p2 != 0 && a.p2 != 1) { reason(e.line, "找图 色校 只能 0/1"); return null; }
                if (a.p1 < 0 || a.p1 > 255) { reason(e.line, "找图 阈值越界（0..255）"); return null; }
                if (!Regions.idOk(a.ref)) { reason(e.line, "模板引用非法：" + a.ref); return null; }
                if (!a.region.isEmpty() && !Regions.idOk(a.region)) { reason(e.line, "区域引用非法：" + a.region); return null; }
                return a;
            }
            if (c.name.equals("找色")) {
                if (c.args.size() != 1 || !(c.args.get(0) instanceof VtAst.StrEx)) { reason(e.line, "找色 参数不匹配（块视图）"); return null; }
                a.jt = 3; a.p1 = 1; a.p2 = 0; a.kms = 0;
                a.ref = strOf((VtAst.StrEx) c.args.get(0), e.line);
                if (a.ref == null) return null;
                for (VtAst.Opt o : c.opts) {
                    if (o.key.equals("区域")) {
                        if (!(o.val instanceof VtAst.StrEx)) { reason(o.line, "找色 区域= 须为字符串字面量"); return null; }
                        a.region = strOf((VtAst.StrEx) o.val, o.line);
                        if (a.region == null) return null;
                    } else { reason(o.line, "找色不支持参数 " + o.key); return null; }
                }
                if (!Regions.idOk(a.ref)) { reason(e.line, "点集引用非法：" + a.ref); return null; }
                if (!a.region.isEmpty() && !Regions.idOk(a.region)) { reason(e.line, "区域引用非法：" + a.region); return null; }
                return a;
            }
            if (c.name.equals("找色点")) {
                if (c.args.size() != 1 || !(c.args.get(0) instanceof VtAst.NumEx)) { reason(e.line, "找色点 参数不匹配（块视图）"); return null; }
                VtAst.NumEx n = (VtAst.NumEx) c.args.get(0);
                long color = (long) n.v;
                if (color < 0 || color > 0xFFFFFF) { reason(e.line, "找色点 颜色越界（0x000000..0xFFFFFF）"); return null; }
                a.jt = 3; a.p1 = 0; a.p2 = 0; a.kms = 0;
                int tol = 8;
                for (VtAst.Opt o : c.opts) {
                    if (o.key.equals("容差")) {
                        tol = litInt(o.val, false, o.line);
                        if (tol == BAD) return null;
                    } else if (o.key.equals("区域")) {
                        if (!(o.val instanceof VtAst.StrEx)) { reason(o.line, "找色点 区域= 须为字符串字面量"); return null; }
                        a.region = strOf((VtAst.StrEx) o.val, o.line);
                        if (a.region == null) return null;
                    } else { reason(o.line, "找色点不支持参数 " + o.key); return null; }
                }
                if (tol < 0 || tol > 255) { reason(e.line, "找色点 容差越界（0..255）"); return null; }
                a.pack = ((int) color << 8) | (tol & 0xFF);
                if (!a.region.isEmpty() && !Regions.idOk(a.region)) { reason(e.line, "区域引用非法：" + a.region); return null; }
                return a;
            }
            reason(e.line, "条件含块视图不支持的函数（" + c.name + "）");
            return null;
        }
        if (e instanceof VtAst.BinEx) {
            VtAst.BinEx b = (VtAst.BinEx) e;
            int pack = -1;
            if (b.op.equals(">")) pack = 0;
            else if (b.op.equals("<")) pack = 1;
            else if (b.op.equals(">=")) pack = 2;
            else if (b.op.equals("<=")) pack = 3;
            else if (b.op.equals("==")) pack = 4;
            else if (b.op.equals("!=")) pack = 5;
            if (pack < 0) { reason(e.line, "条件含块视图不支持的运算（" + b.op + "）"); return null; }
            if (!(b.l instanceof VtAst.IdEx)) { reason(e.line, "变量比较左侧须为变量（块视图）"); return null; }
            int code = varCode(((VtAst.IdEx) b.l).name);
            if (code == BAD) { reason(e.line, "变量无效：" + ((VtAst.IdEx) b.l).name); return null; }
            int rhs = litInt(b.r, true, e.line);              // 参照值 = 字面整数（v6C 口径）
            if (rhs == BAD) return null;
            Atom a = new Atom();
            a.jt = 4;
            a.p1 = code;
            a.p2 = rhs;
            a.pack = pack;
            return a;
        }
        reason(e.line, "条件超出块视图支持（须为判定函数或变量比较）");
        return null;
    }

    /** 字符串字面量 → 纯文本（不含插值；块位只收纯串）。 */
    private String strOf(VtAst.StrEx s, int line) {
        StringBuilder b = new StringBuilder();
        for (Object seg : s.segs) {
            if (seg instanceof String) b.append((String) seg);
            else { reason(line, "该处字符串不支持插值（块视图）"); return null; }
        }
        return b.toString();
    }

    /** 原子 → 步骤（用于头/子条件）。 */
    private static Ops.Step atomStep(int type, Atom a) {
        Ops.Step st = new Ops.Step();
        st.type = type;
        if (type == Ops.T_IF) {
            st.a1 = a.jt; st.a2 = a.p1; st.a3 = a.p2; st.a4 = a.inv;
        } else {                                            // T_CLAUSE
            st.a2 = a.jt; st.a3 = a.p1; st.a4 = a.p2; st.j2 = a.inv;
        }
        st.ms = a.kms;
        st.ref = a.ref == null ? "" : a.ref;
        st.j1 = a.pack;
        st.expr = a.region == null ? "" : a.region;
        return st;
    }
}
