package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * steps → vt-script 翻译器（V8 T5.1）。
 *
 * 输入 = 一条操作的 v5 步骤表（Ops.Op；23 类）→ 输出 = vt-script 源文本（规范形态，SPEC §8）。
 * 语义依据（逐处回核过核心源码）：
 *  - 四档重写（SPEC §9）：继续=该侧空块；跳过=吸收下一步（`如果 c { X }` / `如果 !c { X }`）；
 *    跳转：目标=0 → `终止`；=下一步 → 继续；=下下步 → 跳过；块内可展开（循环界→跳出 k / 循环头→继续 k）；其余不可表达（报告）。
 *  - 组合判定（op_ctrl_eval_list：v6B §3.1）：逐条求值、每题值后取反、且优先于或（短路按取反后的值）→ DNF（组间 ||、组内 &&）。
 *  - 12 模式3 = 成立即退（首判）→ `直到 <组合式>`；14 = 成立进体 → `如果 <组合式>`。
 *  - 找色单点打包 (颜色<<8)|容差：平铺步（11）在 a2；条件位（14/19/12m3 jt3 的 pack）在 j1（op_ctrl_eval 调用口径）。
 *  - 兼容跳转（`跳转 第N行`）仅限顶层语句起始行（VtCheck）→ 转换器预检 + 译后自检。
 * 纪律：一操作一体原子 —— 任何不可译项逐条进 problems、src=null（不做近似、不猜语义）。
 * 纯 Java、零 Android 依赖（离机核 VtFromStepsCheck 直跑）。
 */
public final class VtFromSteps {

    /** 翻译结果。src == null 且 skip == false 时看 problems。 */
    public static final class R {
        public String src;                        // 翻译产物（problems 非空 → null）
        public boolean skip;                      // true = 已是 v8 程序操作（无需翻译，不算失败）
        public final List<String> problems = new ArrayList<String>();
        public final List<String> notes = new ArrayList<String>();   // 非失败信息（如旧数据未名槽自动命名）
    }

    public static R translate(Ops.Op op) { return new VtFromSteps().go(op); }

    /* ================= 内部状态 ================= */

    private List<Ops.Step> steps;
    private String[] slots;
    private int opN;
    private List<String> lines;
    private List<String> problems;
    private List<String> notes;
    private String[] autoNames;
    private int[] stepLine;                                            // 步 → 产出语句首行（1 基）-1 = 无
    private boolean[] stepTop;                                         // 该首行是否顶层
    private final List<J> jumps = new ArrayList<J>();
    private final List<Integer> loops = new ArrayList<Integer>();      // 循环头步下标（外→内）
    private int depth;

    private static final class J { int line, jumpStep, targetStep, targetNo; }

    /** 侧动作（四档求值的去向）。 */
    private static final class Act {
        static final int FALL = 0, SKIP = 1, ABORT = 2, END = 3, CTL = 4, BAD = 5;
        int kind; String ctl;
    }

    private static final String[] PRE = {"tdx", "tdy", "tux", "tuy", "tms", "fx", "fy"};

    /* ================= 主流程 ================= */

    private R go(Ops.Op op) {
        R r = new R();
        problems = r.problems;
        notes = r.notes;
        autoNames = new String[16];
        if (op.progHash != null) { r.skip = true; return r; }
        steps = op.steps;
        opN = steps.size();
        if (opN == 0) { problems.add("操作没有任何步骤"); return r; }
        slots = op.varSlots();
        stepLine = new int[opN];
        for (int i = 0; i < opN; i++) stepLine[i] = -1;
        stepTop = new boolean[opN];
        lines = new ArrayList<String>();
        depth = 0;
        proc(0, opN);
        if (!problems.isEmpty()) return r;
        for (J j : jumps) {
            if (j.targetStep < 0) { lines.set(j.line, "跳转 结束"); continue; }
            if (j.targetStep == j.jumpStep) { problems.add(stepRef(j.jumpStep) + "跳转目标为自身"); continue; }
            if (j.targetStep >= opN || stepLine[j.targetStep] < 0 || !stepTop[j.targetStep]) {
                problems.add(stepRef(j.jumpStep) + "跳转 → 第 " + j.targetNo + " 步：不可表达（目标非顶层语句起始行 / 已被吸收 / 目标在块内）");
                continue;
            }
            lines.set(j.line, "跳转 第" + stepLine[j.targetStep] + "行");
        }
        dedupe(problems);
        if (!problems.isEmpty()) return r;
        StringBuilder sb = new StringBuilder();
        for (String l : lines) sb.append(l).append('\n');
        String src0 = sb.toString();
        VtParser.R pp = VtParser.parse(src0);
        if (pp.err != null) { problems.add("译后自检失败（语法 第 " + pp.line + " 行: " + pp.err + "）"); return r; }
        String src = VtPrint.print(pp.file);                       // 规范形态（表达式空格归一；行数不变，跳转行号保持有效）
        VtCheck.R ck = VtCheck.run(src);
        if (ck.err != null) { problems.add("译后自检失败（检查 第 " + ck.line + " 行: " + ck.err + "）"); return r; }
        VtCompiler.R cp = VtCompiler.compile(src);
        if (cp.err != null) { problems.add("译后自检失败（编译: " + cp.err + "）"); return r; }
        r.src = src;
        return r;
    }

    /* ================= 语句序列处理 ================= */

    /** 处理 [from, to) 区间；返回下一个未消费下标（失败时返回 to）。 */
    private int proc(int from, int to) {
        int i = from;
        while (i < to) {
            int nx = procOne(i, to);
            if (nx <= i) return to;
            i = nx;
        }
        return i;
    }

    /** 处理单个语句单元（吸收链在单元内递归；返回下一未消费下标；失败返回 to）。 */
    private int procOne(int i, int to) {
        Ops.Step s = steps.get(i);
        if (s.raw != null) { problems.add(stepRef(i) + "未知类型/解析失败行，不可翻译"); return to; }
        switch (s.type) {
                case 1:
                    emit("点按 " + num(s.a1, i) + " " + num(s.a2, i) + " " + num(s.ms, i), i); return i + 1;
                case 2: {
                    String t = "滑动 " + num(s.a1, i) + " " + num(s.a2, i) + " " + num(s.a3, i) + " " + num(s.a4, i)
                            + " 时长=" + num(s.ms, i);
                    if (s.j1 != 0) t += " 停1=" + num(s.j1, i);
                    if (s.j2 != 0) t += " 停2=" + num(s.j2, i);
                    emit(t, i); return i + 1;
                }
                case 3: emit("等待 " + num(s.ms, i), i); return i + 1;
                case 4: emit("按下 " + num(s.a1, i) + " " + num(s.a2, i), i); return i + 1;
                case 5: emit("弹起", i); return i + 1;
                case 9: {
                    String nm;
                    if (s.ref == null || s.ref.isEmpty()) {
                        nm = autoName(s.a1, i);          // 旧数据缺名（迁移遗留）→ 沿用 r+槽号 约定自动命名
                        if (nm == null) return to;
                    } else {
                        nm = scriptName(s.ref, i, "变量名");
                    }
                    if (s.expr == null || s.expr.trim().isEmpty()) { problems.add(stepRef(i) + "计算步表达式为空"); return to; }
                    emit(nm + " = " + s.expr.trim(), i); return i + 1;
                }
                case 17: case 18: {
                    int lv = s.a1 == 0 ? 1 : s.a1;                 // v6B：0 = 旧数据视作 1
                    String w = (s.type == 17 ? "跳出" : "继续");
                    if (lv > 1) w += " " + lv;
                    emit(w, i); return i + 1;
                }
                case 20: emit(s.a1 == 1 ? "终止 中止" : "终止", i); return i + 1;
                case 21: {
                    String t = "提示 \"" + toastStr(s.expr, i) + "\"";
                    if (s.ms != 0) t += " 时长=" + num(s.ms, i);
                    emit(t, i); return i + 1;
                }
                case 22: {
                    String t = "框区域 \"" + esc(s.ref) + "\"";
                    if (s.ms != 0) t += " 时长=" + num(s.ms, i);
                    emit(t, i); return i + 1;
                }
                case 23: {
                    String t = "框矩形 " + num(s.a1, i) + " " + num(s.a2, i) + " " + num(s.a3, i) + " " + num(s.a4, i);
                    if (s.ms != 0) t += " 时长=" + num(s.ms, i);
                    emit(t, i); return i + 1;
                }
                case 8: {
                    int tgt = s.a1;
                    if (depth > 0) {
                        String ctl = blockJumpCtrl(tgt, i);
                        if (ctl == null) { problems.add(stepRef(i) + "跳转 → " + tgtRef(tgt) + "：不可表达（块内跳转仅可展开为 跳出/继续/终止）"); return to; }
                        emit(ctl, i);
                    } else if (tgt == 0) {
                        emit("跳转 结束", i);
                    } else {
                        J j = new J();
                        j.line = lines.size();
                        j.jumpStep = i;
                        j.targetStep = tgt - 1;
                        j.targetNo = tgt;
                        jumps.add(j);
                        emit("跳转 §T§", i);
                    }
                    return i + 1;
                }
                case 12: {
                    int tail = Ops.pairTail(steps, i);
                    if (tail < 0 || tail >= to) { problems.add(stepRef(i) + "循环未闭合"); return to; }
                    int bodyStart = Ops.clauseTail(steps, i) + 1;
                    if (s.a1 == 3) {
                        String comb = combined(i, false);
                        if (comb == null) return to;
                        emit("直到 " + comb + " {", i);
                    } else if (s.a1 == 0) {
                        if (bodyStart > i + 1) { problems.add(stepRef(i) + "非「到条件」循环带子条件（数据非法）"); return to; }
                        emit("循环 " + s.a2 + " {", i);
                    } else if (s.a1 == 1) {
                        if (bodyStart > i + 1) { problems.add(stepRef(i) + "非「到条件」循环带子条件（数据非法）"); return to; }
                        emit("循环 时长=" + s.a2 + " {", i);
                    } else {
                        if (bodyStart > i + 1) { problems.add(stepRef(i) + "非「到条件」循环带子条件（数据非法）"); return to; }
                        emit("循环 无限 {", i);
                    }
                    depth++; loops.add(i);
                    proc(bodyStart, tail);
                    loops.remove(loops.size() - 1); depth--;
                    emit("}", -1);
                    return tail + 1;
                }
                case 14: {
                    int tail = Ops.pairTail(steps, i);
                    if (tail < 0 || tail >= to) { problems.add(stepRef(i) + "条件块未闭合"); return to; }
                    int elseIdx = findElse(i, tail);
                    int bodyStart = Ops.clauseTail(steps, i) + 1;
                    String comb = combined(i, true);
                    if (comb == null) return to;
                    emit("如果 " + comb + " {", i);
                    depth++;
                    proc(bodyStart, elseIdx < 0 ? tail : elseIdx);
                    depth--;
                    if (elseIdx >= 0) {
                        emit("} 否则 {", -1);
                        depth++;
                        proc(elseIdx + 1, tail);
                        depth--;
                    }
                    emit("}", -1);
                    return tail + 1;
                }
                case 6: case 7: case 10: case 11: {
                    int nx = condFlat(i, to);
                    if (nx < 0) return to;
                    return nx;
                }
                case 13: case 15: case 16: case 19:
                    problems.add(stepRef(i) + "结构行孤立（无配对块头）");
                    return to;
                default:
                    problems.add(stepRef(i) + "类型 " + s.type + " 不可翻译");
                    return to;
            }
    }

    /** 找本层「否则」（15）：跳嵌套块；无 = -1。 */
    private int findElse(int head, int tail) {
        int d = 0;
        for (int k = head + 1; k < tail; k++) {
            int t = steps.get(k).type;
            if (t == 12 || t == 14) d++;
            else if (t == 13 || t == 16) d--;
            else if (t == 15 && d == 0) return k;
        }
        return -1;
    }

    /* ================= 条件步（6/7/10/11）四档重写 ================= */

    /** 返回下一未消费下标；<0 = 失败。 */
    private int condFlat(int i, int to) {
        Ops.Step s = steps.get(i);
        String c = flatCond(i);
        if (c == null) return -1;
        Act a = sideAct(s.a4, s.j1, i, "成立");
        Act b = sideAct(s.a3, s.j2, i, "不成立");
        if (a == null || b == null) return -1;
        boolean xAbsent = (i + 1 >= to);
        if (xAbsent) { a = skipToFall(a); b = skipToFall(b); }
        boolean af = a.kind == Act.FALL, bf = b.kind == Act.FALL;
        if (af && bf) {
            emit("如果 " + c + " {", i);
            emit("}", -1);
            return i + 1;
        }
        String aTxt = actText(a), bTxt = actText(b);
        if (aTxt == null || bTxt == null) return -1;
        if (af) {
            emit("如果 " + c + " {", i);
            depth++;
            int nx = procOne(i + 1, to);
            depth--;
            if (bTxt.isEmpty()) {
                emit("}", -1);
            } else {
                emit("} 否则 {", -1);
                depth++; emit(bTxt, -1); depth--;
                emit("}", -1);
            }
            return nx;
        }
        if (bf) {
            emit("如果 !" + par(c) + " {", i);
            depth++;
            int nx = procOne(i + 1, to);
            depth--;
            if (aTxt.isEmpty()) {
                emit("}", -1);
            } else {
                emit("} 否则 {", -1);
                depth++; emit(aTxt, -1); depth--;
                emit("}", -1);
            }
            return nx;
        }
        // 两侧都不落空：X 丢弃（旧语义两侧都跳过 / 中止，带不到 X）
        if (aTxt.isEmpty() && bTxt.isEmpty()) {
            emit("如果 " + c + " {", i);
            emit("}", -1);
        } else if (bTxt.isEmpty()) {
            emit("如果 " + c + " {", i);
            depth++; emit(aTxt, -1); depth--;
            emit("}", -1);
        } else {
            emit("如果 " + c + " {", i);
            depth++; if (!aTxt.isEmpty()) emit(aTxt, -1); depth--;
            emit("} 否则 {", -1);
            depth++; emit(bTxt, -1); depth--;
            emit("}", -1);
        }
        return xAbsent ? i + 1 : i + 2;
    }

    private String flatCond(int i) {
        Ops.Step s = steps.get(i);
        if (s.type == 6) return condExpr(0, s.a1, s.a2, 0, s.ref, "", 0, i);
        if (s.type == 7) return condExpr(1, 0, 0, 0, s.ref, "", 0, i);
        if (s.type == 10) return condExpr(2, s.a1, s.a2, s.ms, s.ref, s.expr, 0, i);
        return condExpr(3, s.a1, 0, s.ms, s.ref, s.expr, s.a2, i);     // 11：pack = a2
    }

    /** 侧动作（tier：0 中止 / 1 跳过 / 2 继续 / 3 跳转；target = 跳转目标步号，0=结束）。 */
    private Act sideAct(int tier, int target, int i, String sideName) {
        Act a = new Act();
        if (tier == 2) { a.kind = Act.FALL; return a; }
        if (tier == 1) { a.kind = Act.SKIP; return a; }
        if (tier == 0) { a.kind = Act.ABORT; return a; }
        if (tier == 3) {
            if (target == 0) { a.kind = Act.END; return a; }           // 跳转 0 ≡ 终止（完成收工，不占跳转计数）
            if (target == i + 2) { a.kind = Act.FALL; return a; }      // 跳下一步（1 基步号 = i+2）≡ 继续
            if (target == i + 3) { a.kind = Act.SKIP; return a; }      // 跳下下步（i+3）≡ 跳过下一步
            String ctl = blockJumpCtrl(target, i);
            if (ctl != null) { a.kind = Act.CTL; a.ctl = ctl; return a; }
            problems.add(stepRef(i) + sideName + "侧「跳转 → " + tgtRef(target) + "」不可表达（跳转仅限顶层语句，无法置于分支块内）");
            return null;
        }
        problems.add(stepRef(i) + "条件档位非法 " + tier);
        return null;
    }

    private Act skipToFall(Act a) {
        if (a != null && a.kind == Act.SKIP) { Act r = new Act(); r.kind = Act.FALL; return r; }
        return a;
    }

    private String actText(Act a) {
        switch (a.kind) {
            case Act.FALL: case Act.SKIP: return "";
            case Act.ABORT: return "终止 中止";
            case Act.END: return "终止";
            case Act.CTL: return a.ctl;
        }
        return null;
    }

    /** 块内跳转展开：0→终止 / 循环界→跳出 k / 循环头→继续 k；null = 不可展开。 */
    private String blockJumpCtrl(int tgtNo, int i) {
        if (tgtNo == 0) return "终止";
        int tgt = tgtNo - 1;
        if (tgt < 0 || tgt >= opN) return null;
        for (int k = loops.size(); k >= 1; k--) {
            int head = loops.get(k - 1);
            int tail = Ops.pairTail(steps, head);
            if (tail < 0) continue;
            int outward = loops.size() - k + 1;                        // 1 = 最内
            if (tgt == tail + 1) return outward == 1 ? "跳出" : "跳出 " + outward;
            if (tgt == head) return outward == 1 ? "继续" : "继续 " + outward;
        }
        return null;
    }

    /* ================= 组合判定（头 + 19 链 → DNF 表达式） ================= */

    private String combined(int head, boolean isIf) {
        int tailC = Ops.clauseTail(steps, head);
        List<String> groups = new ArrayList<String>();
        StringBuilder cur = null;
        for (int k = head; k <= tailC; k++) {
            Ops.Step cs = steps.get(k);
            boolean first = (k == head);
            int jt, p1, p2, kms, pack, join;
            boolean inv;
            if (first) {
                if (isIf) { jt = cs.a1; p1 = cs.a2; p2 = cs.a3; kms = cs.ms; pack = cs.j1; inv = cs.a4 == 1; }
                else { jt = cs.a2; p1 = cs.a3; p2 = cs.a4; kms = cs.ms; pack = cs.j1; inv = cs.j2 == 1; }
                join = 0;
            } else {
                jt = cs.a2; p1 = cs.a3; p2 = cs.a4; kms = cs.ms; pack = cs.j1; inv = cs.j2 == 1;
                join = cs.a1;
            }
            String e = condExpr(jt, p1, p2, kms, cs.ref, cs.expr, pack, k);
            if (e == null) return null;
            if (inv) e = e.endsWith(")") ? ("!" + e) : ("!(" + e + ")");
            if (first) cur = new StringBuilder(e);
            else if (join == 1) { groups.add(cur.toString()); cur = new StringBuilder(e); }
            else cur.append(" && ").append(e);
        }
        groups.add(cur.toString());
        StringBuilder b = new StringBuilder(groups.get(0));
        for (int k = 1; k < groups.size(); k++) b.append(" || ").append(groups.get(k));
        return b.toString();
    }

    /** 判定五类 → 表达式（SPEC §4；字段口径 = op_ctrl_eval）。null = 不可表达。 */
    private String condExpr(int jt, int p1, int p2, int kms, String ref, String region, int pack, int at) {
        if (jt == 0) return "在区域(\"" + esc(ref) + "\", " + num(p1, at) + ", " + num(p2, at) + ")";
        if (jt == 1) return "开关开(\"" + esc(ref) + "\")";
        if (jt == 2) {
            StringBuilder b = new StringBuilder("找图(\"" + esc(ref) + "\"");
            if (region != null && !region.isEmpty()) b.append(", 区域=\"").append(esc(region)).append('"');
            if (p1 != 8) b.append(", 阈值=").append(p1);
            if (p2 == 1) b.append(", 色校=1");
            if (kms > 0) b.append(", 持续=").append(kms);
            return b.append(')').toString();
        }
        if (jt == 3) {
            if (kms > 0) {
                problems.add(stepRef(at) + "找色·持续查找（ms>0）暂不可表达（脚本参数键缺口，见 SPEC §3）");
                return null;
            }
            StringBuilder b;
            if (p1 == 1) {
                b = new StringBuilder("找色(\"" + esc(ref) + "\"");
            } else {
                int color = (pack >>> 8) & 0xFFFFFF, tol = pack & 0xFF;
                b = new StringBuilder("找色点(0x" + String.format("%06X", color));
                if (tol != 8) b.append(", 容差=").append(tol);
            }
            if (region != null && !region.isEmpty()) b.append(", 区域=\"").append(esc(region)).append('"');
            return b.append(')').toString();
        }
        if (jt == 4) {
            if (pack < 0 || pack > 5) { problems.add(stepRef(at) + "比较符非法 " + pack); return null; }
            String[] ops = {">", "<", ">=", "<=", "==", "!="};
            return num(p1, at) + " " + ops[pack] + " " + p2;
        }
        problems.add(stepRef(at) + "判定类型 " + jt + " 不可翻译");
        return null;
    }

    /* ================= 字段 / 名字 / 模板 ================= */

    /** 数值字段 → 文本（负值 = 变量引用；-8/-9 = 退役槽）。 */
    private String num(int v, int i) {
        if (v >= 0) return String.valueOf(v);
        switch (v) {
            case -1: return "tdx"; case -2: return "tdy"; case -3: return "tux"; case -4: return "tuy";
            case -5: return "tms"; case -6: return "fx"; case -7: return "fy";
            case -8: case -9:
                problems.add(stepRef(i) + "退役槽编码 " + v + " 不可翻译");
                return "?";
            default: break;
        }
        if (v >= -25) {
            int idx = -10 - v;
            String nm = (idx >= 0 && idx < slots.length) ? slots[idx] : null;
            if (nm == null || nm.isEmpty()) {
                if (unnamedCalcAt(idx) < 0) {
                    problems.add(stepRef(i) + "自定义变量槽 " + (idx + 1) + " 无名字且无写入步（遗留数据）");
                    return "?";
                }
                nm = autoName(idx, i);                   // 有匿名写入步 → 自动命名（与写端同名）
                if (nm == null) return "?";
                return nm;
            }
            if (!nameOk(nm, i, "变量名")) return "?";
            return nm;
        }
        problems.add(stepRef(i) + "变量编码非法 " + v);
        return "?";
    }

    /** 计算步目标名。 */
    private String scriptName(String nm, int i, String what) {
        if (!nameOk(nm, i, what)) return "?";
        return nm;
    }

    /** 名字 → vt-script 标识符合法性（SPEC §2：可见字符 1..63、首非数字、不含 '-'、非保留字）。 */
    private boolean nameOk(String nm, int i, String what) {
        if (nm == null || nm.isEmpty()) { problems.add(stepRef(i) + what + "为空"); return false; }
        if (nm.length() > 63) { problems.add(stepRef(i) + what + "过长（>63 字符）：" + nm); return false; }
        if (VtTok.kwOf(nm) > 0) { problems.add(stepRef(i) + what + "与保留字重名（需改名）：" + nm); return false; }
        if (nm.indexOf('-') >= 0) { problems.add(stepRef(i) + what + "含 '-'（旧名尺子允许、脚本不允许，需改名）：" + nm); return false; }
        for (int k = 0; k < nm.length(); k++) {
            char c = nm.charAt(k);
            if (c < 0x80) {
                boolean okc = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
                if (!okc) { problems.add(stepRef(i) + what + "含脚本不允许字符 '" + c + "'：" + nm); return false; }
            } else if (Character.isWhitespace(c) || Character.isISOControl(c) || Character.getType(c) == Character.FORMAT) {
                problems.add(stepRef(i) + what + "含空白/控制/格式字符：" + nm); return false;
            }
        }
        char f = nm.charAt(0);
        if (f >= '0' && f <= '9') { problems.add(stepRef(i) + what + "首字符不可为数字：" + nm); return false; }
        return true;
    }

    /** 匿名写入步（计算，a1 == idx 且 ref 空）下标；无 = -1。 */
    private int unnamedCalcAt(int idx) {
        for (int k = 0; k < opN; k++) {
            Ops.Step s = steps.get(k);
            if (s.raw == null && s.type == 9 && s.a1 == idx && (s.ref == null || s.ref.isEmpty())) return k;
        }
        return -1;
    }

    /** 旧数据未名槽 → 自动命名（沿用 translateOld 的 r+槽号 约定；冲突顺延 槽N/变量N）。返回名字或 null（已记问题）。 */
    private String autoName(int idx, int i) {
        if (idx < 0 || idx > 15) { problems.add(stepRef(i) + "自定义变量索引非法 " + idx); return null; }
        if (autoNames == null) autoNames = new String[16];
        if (autoNames[idx] != null) return autoNames[idx];
        int n = idx + 1;
        String[] cands = {"r" + n, "槽" + n, "变量" + n, "槽" + n + "_2", "槽" + n + "_3"};
        for (String c : cands) {
            if (!autoTaken(c, idx)) {
                autoNames[idx] = c;
                notes.add(stepRef(i) + "变量槽 " + n + " 无名字（旧数据）→ 自动命名「" + c + "」（沿用 r+槽号 约定）");
                return c;
            }
        }
        problems.add(stepRef(i) + "变量槽 " + n + " 自动命名失败（候选名全被占用）");
        return null;
    }

    private boolean autoTaken(String c, int self) {
        for (String p : PRE) if (p.equals(c)) return true;
        for (int k = 0; k < slots.length; k++) {
            if (k == self) continue;
            if (slots[k] != null && slots[k].equals(c)) return true;
        }
        if (autoNames != null) {
            for (int k = 0; k < autoNames.length; k++) {
                if (k != self && c.equals(autoNames[k])) return true;
            }
        }
        return false;
    }

    /** 提示模板 → 脚本字符串内容（{名} 保留、{{ }} 保留、孤立 } → }}、引号/反斜杠转义；占位名检查）。 */
    private String toastStr(String tpl, int i) {
        if (tpl == null) tpl = "";
        StringBuilder b = new StringBuilder();
        int k = 0;
        while (k < tpl.length()) {
            char c = tpl.charAt(k);
            if (c == '{') {
                if (k + 1 < tpl.length() && tpl.charAt(k + 1) == '{') { b.append("{{"); k += 2; continue; }
                int j2 = tpl.indexOf('}', k + 1);
                if (j2 < 0) { problems.add(stepRef(i) + "提示模板未闭合的 {"); b.append("{{"); k++; continue; }
                String nm = tpl.substring(k + 1, j2);
                preOrSlot(nm, i);
                b.append('{').append(nm).append('}');
                k = j2 + 1;
                continue;
            }
            if (c == '}') {
                if (k + 1 < tpl.length() && tpl.charAt(k + 1) == '}') { b.append("}}"); k += 2; continue; }
                b.append("}}"); k++; continue;                         // 孤立 } → }}（脚本字面 }）
            }
            if (c == '"') { b.append("\\\""); k++; continue; }
            if (c == '\\') { b.append("\\\\"); k++; continue; }
            b.append(c); k++;
        }
        return b.toString();
    }

    private void preOrSlot(String nm, int i) {
        for (String p : PRE) if (p.equals(nm)) return;
        for (String sn : slots) {
            if (sn != null && sn.equals(nm)) { nameOk(nm, i, "模板占位变量名"); return; }
        }
        problems.add(stepRef(i) + "提示占位未知变量 " + nm);
    }

    private static String esc(String v) {
        if (v == null) return "";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < v.length(); i++) {
            char c = v.charAt(i);
            if (c == '"') b.append("\\\"");
            else if (c == '\\') b.append("\\\\");
            else if (c == '\n') b.append("\\n");
            else if (c == '\t') b.append("\\t");
            else b.append(c);
        }
        return b.toString();
    }

    /** 调用的反转形态：调用名后直接 `!`（无空格无括号）；其余（比较式）套括号。 */
    private static String par(String c) {
        if (c.endsWith(")")) return c;
        return "(" + c + ")";
    }

    /* ================= 输出 ================= */

    private void emit(String text, int step) {
        StringBuilder b = new StringBuilder();
        for (int k = 0; k < depth; k++) b.append("    ");
        b.append(text);
        lines.add(b.toString());
        if (step >= 0 && step < opN && stepLine[step] < 0) {
            stepLine[step] = lines.size();
            stepTop[step] = (depth == 0);
        }
    }

    private static void dedupe(List<String> xs) {
        java.util.LinkedHashSet<String> seen = new java.util.LinkedHashSet<String>(xs);
        xs.clear();
        xs.addAll(seen);
    }

    private String stepRef(int i) {
        Ops.Step s = (i >= 0 && i < opN) ? steps.get(i) : null;
        String tn = (s != null && s.type >= 1 && s.type <= 23) ? ("（" + Ops.typeName(s.type) + "）") : "";
        return "第 " + (i + 1) + " 步" + tn;
    }

    private static String tgtRef(int t) {
        return t == 0 ? "结束" : ("第 " + t + " 步");
    }
}
