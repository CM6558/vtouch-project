package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * 步骤剪贴板（v31）：全局单槽 —— 单步 / 整块（含子条件/否则/块尾）/ 子条件行 / 连选区间共用。
 * 口径（拍板）：原样深拷贝（字段一字不改）；跨操作、跨方案存活（进程内静态；退出进程清空）；粘完不清空，可多处粘。
 * 落点合法性 = 试插入 + 结构预检（Ops.structureErr；与「移动」同规则——含跳转的单元按其结构约束过滤）。
 * 粘贴预检 = 轻提示口径：把单元变量引用与目标操作现状对账，返回一句提醒或 null（不自动改任何字段）。
 */
final class StepClip {
    /** 剪贴板内容（深拷贝；null = 空）。 */
    static List<Ops.Step> steps = null;
    /** 展示名（如「6 步」）。 */
    static String label = "";
    /** 复制来源操作名（同操作「原位置之后」快捷项用）。 */
    static String srcOp = "";
    /** 复制时单元首行下标（源操作内）。 */
    static int srcIdx = -1;
    /** 复制时源操作变量槽名表（粘贴预检对比用；可空）。 */
    static String[] srcVars = null;

    private StepClip() { }

    static boolean empty() { return steps == null || steps.isEmpty(); }

    static int size() { return empty() ? 0 : steps.size(); }

    /** 复制 [head..tail] 为剪贴板（调用方保证 tail ≥ head；块头单元由 Ops.unitTail 已含整块）。 */
    static void copy(List<Ops.Step> src, int head, int tail, String opName, String[] varSlots) {
        List<Ops.Step> unit = new ArrayList<Ops.Step>();
        for (int i = head; i <= tail; i++) unit.add(src.get(i).copy());
        steps = unit;
        label = unit.size() + " 步";
        srcOp = opName == null ? "" : opName;
        srcIdx = head;
        srcVars = varSlots;
    }

    /** 剪贴板复位（测试隔离用；正常使用不清——粘完不清空是口径）。 */
    static void reset() { steps = null; label = ""; srcOp = ""; srcIdx = -1; srcVars = null; }

    /** 合法插入落点（0 基插入下标，升序；空 = 无处可粘）。规则 = 试插入 + structureErr 过滤。 */
    static List<Integer> legalPositions(List<Ops.Step> target, List<Ops.Step> unit) {
        List<Integer> out = new ArrayList<Integer>();
        if (target == null || unit == null || unit.isEmpty()) return out;
        if (target.size() + unit.size() > Ops.MAX_STEPS) return out;
        int n = target.size();
        for (int p = 0; p <= n; p++) {
            List<Ops.Step> t = new ArrayList<Ops.Step>(n + unit.size());
            t.addAll(target.subList(0, p));
            t.addAll(unit);
            t.addAll(target.subList(p, n));
            if (Ops.structureErr(t) == null) out.add(Integer.valueOf(p));
        }
        return out;
    }

    /** 粘贴到插入下标 p（越界 = 末尾）：成功 = 就地插入深拷贝（多次粘贴互不影响，剪贴板保留）；失败 = 返回原因。 */
    static String paste(List<Ops.Step> target, int p) {
        if (empty()) return "剪贴板为空";
        if (target == null) return "没有目标操作";
        if (p < 0 || p > target.size()) p = target.size();
        if (target.size() + steps.size() > Ops.MAX_STEPS)
            return "粘贴将超过上限 " + Ops.MAX_STEPS + "（当前 " + target.size() + " + 剪贴板 " + steps.size() + " 步）";
        List<Ops.Step> t = new ArrayList<Ops.Step>(target.size() + steps.size());
        t.addAll(target.subList(0, p));
        t.addAll(steps);
        t.addAll(target.subList(p, target.size()));
        String err = Ops.structureErr(t);
        if (err != null) return "落点不合法：" + err;
        for (int i = 0; i < steps.size(); i++) target.add(p + i, steps.get(i).copy());
        return null;
    }

    /** 连选区间归一（v31：块边界自动补全）：把 [a,b] 扩展到完整结构——
     *  含块头（12/14）→ 扩到配对尾；含块尾/否则/子条件 → 扩到所属块头；子条件链随头走。
     *  返回 {s,e}（含端点，0 基）或 null（结构破损，拒绝连选）。 */
    static int[] normalizeRange(List<Ops.Step> src, int a, int b) {
        int n = src.size();
        if (n == 0) return null;
        int s = Math.min(a, b), e = Math.max(a, b);
        if (s < 0) s = 0;
        if (e >= n) e = n - 1;
        if (s > e) return null;
        boolean changed = true;
        int guard = 0;
        while (changed && guard++ < 32) {
            changed = false;
            for (int i = s; i <= e && i < n; i++) {
                int t = src.get(i).type;
                if (t == Ops.T_LOOP || t == Ops.T_IF) {
                    int tt = Ops.pairTail(src, i);
                    if (tt < 0) return null;
                    if (tt > e) { e = tt; changed = true; }
                } else if (t == Ops.T_ENDLOOP || t == Ops.T_ENDIF || t == Ops.T_ELSE || t == Ops.T_CLAUSE) {
                    int h = Ops.blockHeadOf(src, i);
                    if (h < 0) return null;
                    if (h < s) { s = h; changed = true; }
                }
            }
        }
        return new int[]{s, e};
    }

    /** 粘贴前轻预检（原样复制口径）：把单元变量引用与目标操作现状对账，返回提醒文案（如「变量引用 2 处需调整」）
     *  或 null（无问题）。不自动改字段。覆盖：① 单元计算步槽位被目标占用（异名）/ 与目标变量重名；
     *  ② 负编码引用的槽在「目标∪单元」里无名字（悬空）；③ 目标槽名与源操作槽名不符（语义漂移）；
     *  ④ 提示模板 {名} 找不到定义；⑤ 计算步表达式引用了未定义名字。 */
    static String preflightNotice(List<Ops.Step> target, String[] targetVars,
                                  List<Ops.Step> unit, String[] srcVars) {
        if (unit == null || unit.isEmpty()) return null;
        String[] tv = targetVars != null ? targetVars : new String[Ops.VAR_N];
        String[] uv = new String[Ops.VAR_N];
        for (Ops.Step s : unit) {
            if (s.raw == null && s.type == Ops.T_CALC && s.a1 >= 0 && s.a1 < Ops.VAR_N && !s.ref.isEmpty())
                uv[s.a1] = s.ref;
        }
        int issues = 0;
        for (int i = 0; i < Ops.VAR_N; i++) {                       // ① 槽位冲突 / 重名
            String nm = uv[i];
            if (nm == null) continue;
            if (tv[i] != null && !tv[i].equals(nm)) { issues++; continue; }
            for (int k = 0; k < Ops.VAR_N; k++)
                if (k != i && nm.equals(tv[k])) { issues++; break; }
        }
        for (Ops.Step s : unit) {                                   // ②③ 负编码引用对账
            if (s.raw != null) continue;
            int[] f = {s.a1, s.a2, s.a3, s.a4, s.ms, s.j1, s.j2};
            for (int v : f) {
                int ci = Ops.customIndexOf(v);
                if (ci < 0 || uv[ci] != null) continue;
                if (tv[ci] == null) { issues++; continue; }
                if (srcVars != null && ci < srcVars.length && srcVars[ci] != null
                        && !srcVars[ci].equals(tv[ci])) issues++;
            }
        }
        for (Ops.Step s : unit) {                                   // ④ 提示模板占位
            if (s.raw != null || s.type != Ops.T_TOAST) continue;
            for (String nm : tplNames(s.expr)) {
                if (predef(nm) || hasName(uv, nm) || hasName(tv, nm)) continue;
                issues++;
            }
        }
        for (Ops.Step s : unit) {                                   // ⑤ 计算步表达式名字
            if (s.raw != null || s.type != Ops.T_CALC) continue;
            if (s.expr == null || s.expr.isEmpty()) continue;
            if (Expr.check(s.expr, combinedNames(uv, tv)) != null) issues++;
        }
        if (issues <= 0) return null;
        return "变量引用 " + issues + " 处需调整";
    }

    /** 提示模板占位名提取（口径同 Ops.toastTplErr：{{ }} 转义；忽略未闭合）。 */
    private static List<String> tplNames(String tpl) {
        List<String> out = new ArrayList<String>();
        if (tpl == null) return out;
        int i = 0, len = tpl.length();
        while (i < len) {
            char ch = tpl.charAt(i);
            if (ch == '{') {
                if (i + 1 < len && tpl.charAt(i + 1) == '{') { i += 2; continue; }
                int j = tpl.indexOf('}', i + 1);
                if (j < 0) break;
                String nm = tpl.substring(i + 1, j);
                if (!nm.isEmpty()) out.add(nm);
                i = j + 1;
            } else if (ch == '}' && i + 1 < len && tpl.charAt(i + 1) == '}') {
                i += 2;
            } else i++;
        }
        return out;
    }

    /** 预置变量名（同 Ops.varLabel 的 7 个）。 */
    private static boolean predef(String nm) {
        return nm.equals("tdx") || nm.equals("tdy") || nm.equals("tux") || nm.equals("tuy")
                || nm.equals("tms") || nm.equals("fx") || nm.equals("fy");
    }

    private static boolean hasName(String[] v, String nm) {
        for (String x : v) if (nm.equals(x)) return true;
        return false;
    }

    private static List<String> combinedNames(String[] uv, String[] tv) {
        List<String> l = new ArrayList<String>();
        for (String x : uv) if (x != null) l.add(x);
        for (String x : tv) if (x != null && !l.contains(x)) l.add(x);
        return l;
    }
}
