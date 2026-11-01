package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * ops.conf 的模型与读写（v6：对齐仓库 ops v5 / 契约 v10 —— 11 类步骤 + 表达式 + 命名变量）。
 *
 * 格式（仓库 src-ui/vtouch_ui.cpp「操作表落盘」逐字）：
 *   #vtouch-ops v5
 *   op   <名> gate <门控区域|-> autooff <0|1>
 *   step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>
 * ref / expr 空写 "-"（读回还原空串）；变量照写负数（-1..-5 触发、-6/-7 fx/fy、
 * -8/-9 退役槽、-10..-25 自定义变量索引 0..15）；expr 可含空白（行尾整段）。
 * 类型 1..11：点按/滑动/等待/按下/弹起/区域判断/开关判断/跳转/计算/找图/找色；
 * 12..19 控制流结构（循环/条件块/否则/跳出/继续/子条件）+ 20 终止（v6B）。
 * 读端兼容 v1(6 字段)/v2(7)/v3(9)/v4(10)，类型感知翻译 + v1..v4→v5 翻译矩阵（对齐面板 ops_translate_old）；
 * 序列化统一写 v5。未知类型(>11)/解析失败的行逐字保留。
 */
public final class Ops {
    public static final String PATH = "/data/local/vtouch-runtime/ops.conf";
    public static final String TMPL_DIR = "/data/local/vtouch-runtime/templates";

    public static final int T_TAP = 1, T_SWIPE = 2, T_WAIT = 3, T_DOWN = 4, T_UP = 5,
            T_COND_REGION = 6, T_COND_TOGGLE = 7, T_JUMP = 8, T_CALC = 9, T_FINDIMAGE = 10, T_FINDCOLOR = 11,
            T_LOOP = 12, T_ENDLOOP = 13, T_IF = 14, T_ELSE = 15, T_ENDIF = 16, T_BREAK = 17, T_CONT = 18,
            T_CLAUSE = 19, T_STOP = 20, T_TOAST = 21, T_HL = 22, T_BOX = 23;   // v6B：19 子条件（块头绑定）/ 20 终止；v6D：21 提示；v6E：22 框区域；v6F：23 框矩形
    public static final int C_ABORT = 0, C_SKIP = 1, C_CONT = 2, C_JUMP = 3;
    public static final int J_AND = 0, J_OR = 1;   // 子条件连接词（19.a1；v6B）
    public static final int CMP_GT = 0, CMP_LT = 1, CMP_GE = 2, CMP_LE = 3, CMP_EQ = 4, CMP_NE = 5;   // 变量比较符（v6C）
    public static final String[] CMP_SYMS = {">", "<", "≥", "≤", "=", "≠"};   // 与 CMP_* 同序（v6C；内核/编辑器双端同源）
    public static final int CLAUSE_MAX = 4;        // 每块头子条件上限（含头子句；同核心 VT_CTRL_CLAUSE_MAX）

    public static final int MAX_STEPS = 512;
    public static final int VAR_N = 16;          // 自定义变量表条数（编码 -10..-25）
    public static final int VAR_NAME_MAX = 15;   // v11：可见字符 1..15（可用中文；首字符不可数字/"-"）
    public static final int EXPR_MAX = 63;

    // 负数编码（与核心一致）
    public static final int V_TDX = -1, V_TDY = -2, V_TUX = -3, V_TUY = -4, V_TMS = -5,
            V_FX = -6, V_FY = -7, V_CUSTOM0 = -10, V_CUSTOM15 = -25;

    public static String typeName(int t) {
        switch (t) {
            case 1: return "点按"; case 2: return "滑动"; case 3: return "等待";
            case 4: return "按下"; case 5: return "弹起"; case 6: return "区域判断";
            case 7: return "开关判断"; case 8: return "跳转"; case 9: return "计算";
            case 10: return "找图"; case 11: return "找色";
            case 12: return "循环"; case 13: return "循环结束"; case 14: return "条件块";
            case 15: return "否则"; case 16: return "条件块结束"; case 17: return "跳出循环"; case 18: return "继续循环";
            case 19: return "子条件"; case 20: return "终止"; case 21: return "提示"; case 22: return "框区域"; case 23: return "框矩形";
            default: return "未知类型" + t;
        }
    }

    public static String tierName(int t) {
        switch (t) {
            case 0: return "中止"; case 1: return "跳过下一步";
            case 2: return "继续下一步"; case 3: return "跳转";
            default: return "?" + t;
        }
    }

    /** 自定义变量名合法性（v11 同核心 op_var_name_ok：可见字符 1..15；首字符不可数字/"-"）。 */
    public static boolean varNameOk(String s) {
        return Regions.varOk(s);
    }

    /** 负数编码是不是自定义变量引用；是 → 返回索引 0..15，否则 -1。 */
    public static int customIndexOf(int v) {
        return (v >= V_CUSTOM15 && v <= V_CUSTOM0) ? (-v - 10) : -1;
    }

    public static int customCode(int idx) { return V_CUSTOM0 - idx; }

    public static final class Step {
        public int type;
        public int a1, a2, a3, a4, ms;
        public String ref = "";
        public int j1, j2;
        public String expr = "";      // 计算步表达式 / 视觉步区域名（空 = 无）
        public String raw = null;     // 未知类型/解析失败：原行逐字

        public boolean known() { return raw == null; }

        public Step copy() {
            Step s = new Step();
            s.type = type; s.a1 = a1; s.a2 = a2; s.a3 = a3; s.a4 = a4; s.ms = ms;
            s.ref = ref; s.j1 = j1; s.j2 = j2; s.expr = expr; s.raw = raw;
            return s;
        }

        public String summary() { return summary(null); }

        /** v6C：带变量槽名表的摘要（变量比较显名字；slots 可空 = 显示编码兜底）。 */
        public String summary(String[] slots) {
            if (raw != null) return "原样保留: " + raw;
            switch (type) {
                case T_TAP: return "点按 " + nv(a1, slots) + "," + nv(a2, slots) + "  " + msCell(ms, slots);
                case T_SWIPE: {
                    String s = "滑动 " + nv(a1, slots) + "," + nv(a2, slots) + " → " + nv(a3, slots) + "," + nv(a4, slots)
                            + "  " + msCell(ms, slots);
                    if (j1 != 0) s += "  按下后停 " + msCell(j1, slots);
                    if (j2 != 0) s += "  弹起前停 " + msCell(j2, slots);
                    return s;
                }
                case T_WAIT: return "等待 " + msCell(ms, slots);
                case T_DOWN: return "按下 " + nv(a1, slots) + "," + nv(a2, slots);
                case T_UP: return "弹起";
                case T_COND_REGION:
                    return "区域判断 " + ref + "  成立:" + sideText(a4, j1) + " / 不成立:" + sideText(a3, j2);
                case T_COND_TOGGLE:
                    return "开关判断 " + ref + "  成立:" + sideText(a4, j1) + " / 不成立:" + sideText(a3, j2);
                case T_JUMP: return "跳转 → " + (a1 == 0 ? "结束" : "第 " + a1 + " 步");
                case T_CALC: return "计算 → " + ref + " = " + trunc(expr);
                case T_FINDIMAGE: {
                    String s = "找图 " + ref + "  区域:" + (expr.isEmpty() ? "全屏" : expr) + "  阈值:" + a1;
                    if (a2 == 1) s += " +色校";
                    if (ms > 0) s += "  持续≤" + ms + "ms";
                    return s + "  成立:" + sideText(a4, j1) + " / 不成立:" + sideText(a3, j2);
                }
                case T_FINDCOLOR: {
                    String s;
                    if (a1 == 1) s = "找色 多点 " + ref;
                    else s = "找色 单点 #" + String.format("%06X", (a2 >>> 8) & 0xFFFFFF) + " 容差" + (a2 & 0xFF);
                    s += "  区域:" + (expr.isEmpty() ? "全屏" : expr);
                    if (ms > 0) s += "  持续≤" + ms + "ms";
                    return s + "  成立:" + sideText(a4, j1) + " / 不成立:" + sideText(a3, j2);
                }
                case T_LOOP: {
                    String m;
                    if (a1 == 0) m = "次数 " + a2;
                    else if (a1 == 1) m = "持续 " + durText(a2);
                    else if (a1 == 2) m = "无限";
                    else m = "到条件 · " + condText(a2, ref, a3, a4, j1, expr, slots);
                    return "循环 · " + m;
                }
                case T_ENDLOOP: return "循环结束";
                case T_IF: return "如果 · " + condText(a1, ref, a2, a3, j1, expr, slots);
                case T_ELSE: return "否则";
                case T_ENDIF: return "条件块结束";
                case T_BREAK: return a1 > 1 ? ("跳出循环（" + a1 + " 层）") : "跳出循环";
                case T_CONT: return a1 > 1 ? ("继续循环（" + a1 + " 层）") : "继续循环（跳过本轮剩余 → 下一轮）";
                case T_CLAUSE: return clauseSummary(slots);
                case T_STOP: return a1 == 1 ? "终止 · 中止退出" : "终止 · 完成收工";
                case T_TOAST: return "提示 · " + trunc(expr == null ? "" : expr);
                case T_HL: return "框区域 · " + (ref == null ? "" : ref) + "（" + Math.round((ms > 0 ? ms : 3000) / 1000f) + " 秒）";
                case T_BOX: return "框矩形 (" + nv(a1, slots) + "," + nv(a2, slots) + ")–(" + nv(a3, slots) + "," + nv(a4, slots)
                        + ")（" + Math.round((ms > 0 ? ms : 3000) / 1000f) + " 秒）";
                default: return "未知类型" + type;
            }
        }

        /** 子条件行摘要（19；v6B/v6C）：`且 · 非 开关判断 s1` 形态。 */
        public String clauseSummary() { return clauseSummary(null); }

        /** v6C：带变量槽名表版。 */
        public String clauseSummary(String[] slots) {
            return (a1 == J_OR ? "或" : "且") + " · " + clauseBody(slots);
        }

        /** 子条件本体（不含连接词；v6B）：`非 开关判断 s1` 形态。 */
        public String clauseBody() { return clauseBody(null); }

        /** v6C：带变量槽名表版。 */
        public String clauseBody(String[] slots) {
            return (j2 == 1 ? "非 " : "") + condText(a2, ref, a3, a4, j1, expr, slots);
        }

        /** 循环时长摘要（1000..86400000 ms）：整分/整时走 分/时，其余 ms。 */
        private static String durText(int ms) {
            if (ms >= 60000 && ms % 60000 == 0) {
                int m = ms / 60000;
                if (m >= 60 && m % 60 == 0) return (m / 60) + " 小时";
                return m + " 分钟";
            }
            return ms + "ms";
        }

        /** 控制流判定摘要（循环·到条件 / 条件块共用；字段位置由调用方投影，v12；v6C 加变量比较）。 */
        private static String condText(int jt, String ref, int p1, int p2, int pack, String region) {
            return condText(jt, ref, p1, p2, pack, region, null);
        }

        /** v6C：带变量槽名表版（变量比较显变量名而非编码）。 */
        private static String condText(int jt, String ref, int p1, int p2, int pack, String region, String[] slots) {
            String base;
            switch (jt) {
                case 0: base = "区域判断 " + ref + " @" + nv(p1, slots) + "," + nv(p2, slots); break;
                case 1: base = "开关判断 " + ref; break;
                case 2: base = "找图 " + ref + " 阈值" + p1; break;
                case 3: base = (p1 == 1) ? ("找色 多点 " + ref)
                                          : ("找色 单点 #" + String.format("%06X", (pack >>> 8) & 0xFFFFFF)); break;
                default: base = "变量 " + varLabel(p1, slots) + " " + cmpSym(pack) + " " + p2; break;
            }
            if (region != null && !region.isEmpty()) base += " [" + region + "]";
            return base;
        }

        /** v32：数值字段显示——负值 = 变量引用，显变量名而非编码（列表/总览/摘要统一口径）。 */
        static String nv(int v, String[] slots) {
            return v < 0 ? varLabel(v, slots) : String.valueOf(v);
        }

        /** v32：毫秒字段（变量引用时不再缀 ms）。 */
        static String msCell(int v, String[] slots) {
            return v < 0 ? varLabel(v, slots) : v + "ms";
        }

        /** 变量码 → 显示名（v6C）：-1..-7 预置名；-10..-25 优先槽名（无则「变量 N」）；其余显码值。 */
        static String varLabel(int code, String[] slots) {
            switch (code) {
                case -1: return "tdx"; case -2: return "tdy"; case -3: return "tux"; case -4: return "tuy";
                case -5: return "tms"; case -6: return "fx"; case -7: return "fy";
                case -8: case -9: return "退役槽";
                default:
                    if (code >= -25 && code <= -10) {
                        int idx = -10 - code;
                        if (slots != null && idx >= 0 && idx < slots.length && slots[idx] != null) return slots[idx];
                        return "变量" + (idx + 1);
                    }
                    return String.valueOf(code);
            }
        }

        /** 比较符码 → 符号（v6C；域外防御显示 ?）。 */
        static String cmpSym(int pack) {
            return (pack >= 0 && pack < CMP_SYMS.length) ? CMP_SYMS[pack] : "?";
        }

        private static String trunc(String e) {
            return e.length() <= 32 ? e : e.substring(0, 31) + "…";
        }

        private static String sideText(int tier, int target) {
            return tierName(tier) + (tier == C_JUMP ? ("→" + (target == 0 ? "结束" : target)) : "");
        }
    }

    public static final class Op {
        public String name = "";
        public String gate = "-";
        public int autooff = 0;
        public String extra = null;   // op 行第 6 字段起（原样保留，正常为 null）
        public final List<Step> steps = new ArrayList<Step>();

        public Op copy() {
            Op o = new Op();
            o.name = name; o.gate = gate; o.autooff = autooff; o.extra = extra;
            for (Step s : steps) o.steps.add(s.copy());
            return o;
        }

        /** 变量名表（索引 0..15；空槽 = null）—— 由计算步派生（对齐核心 vars[]，a1 位即名字）。 */
        public String[] varSlots() {
            String[] v = new String[VAR_N];
            for (Step s : steps) {
                if (s.raw == null && s.type == T_CALC && s.a1 >= 0 && s.a1 < VAR_N && !s.ref.isEmpty()) {
                    v[s.a1] = s.ref;
                }
            }
            return v;
        }

        /** 非空自定义变量名（按索引序；供表达式校验与变量选择器）。 */
        public List<String> varNames() {
            List<String> l = new ArrayList<String>();
            for (String s : varSlots()) if (s != null) l.add(s);
            return l;
        }
    }

    public static final class File {
        public int version = 0;
        public String header = null;
        public final List<Op> ops = new ArrayList<Op>();
        public final List<String> unknown = new ArrayList<String>();

        public Op find(String name) {
            for (Op o : ops) if (o.name.equals(name)) return o;
            return null;
        }

        public static File parse(String text) {
            File f = new File();
            Op current = null;
            for (String rawLine : text.split("\n")) {
                String line = rawLine.trim();
                if (line.isEmpty()) continue;
                if (line.startsWith("#vtouch-ops")) {
                    f.header = line;
                    String v = line.replaceAll("[^0-9]", "");
                    f.version = v.isEmpty() ? 0 : Integer.parseInt(v);
                    continue;
                }
                String[] t = line.split("\\s+");
                if (t[0].equals("op") && t.length >= 6 && t[2].equals("gate") && t[4].equals("autooff")) {
                    Op o = new Op();
                    o.name = t[1]; o.gate = t[3]; o.autooff = pi(t[5]);
                    if (t.length > 6) {
                        StringBuilder sb = new StringBuilder();
                        for (int i = 6; i < t.length; i++) { if (i > 6) sb.append(' '); sb.append(t[i]); }
                        o.extra = sb.toString();
                    }
                    f.ops.add(o);
                    current = o;
                } else if (t[0].equals("step") && current != null && t.length >= 7) {
                    Step s = parseStep(line, t);
                    if (s != null) current.steps.add(s);
                    else f.unknown.add(line);
                } else {
                    f.unknown.add(line);
                }
            }
            if (f.header == null) throw new IllegalArgumentException("操作文件缺少 #vtouch-ops 版本行");
            if (f.version >= 1 && f.version < 5) f.translateOld();   // v1..v4 → v5（对齐面板翻译矩阵）
            return f;
        }

        /** 一行 step（token 数定版本：7=v1、8=v2、10=v3、>=11=v4/v5，expr 取行尾整段）。 */
        private static Step parseStep(String line, String[] t) {
            try {
                int n = t.length;
                Step s = new Step();
                s.type = pi(t[1]);
                s.a1 = pi(t[2]); s.a2 = pi(t[3]); s.a3 = pi(t[4]); s.a4 = pi(t[5]); s.ms = pi(t[6]);
                if (n >= 8) s.ref = t[7].equals("-") ? "" : t[7];
                if (n >= 10) { s.j1 = pi(t[8]); s.j2 = pi(t[9]); }
                if (n >= 11) {
                    StringBuilder sb = new StringBuilder();
                    for (int i = 10; i < n; i++) { if (i > 10) sb.append(' '); sb.append(t[i]); }
                    String e = sb.toString().trim();
                    s.expr = e.equals("-") ? "" : e;
                }
                // 类型感知翻译：旧行（v1/v2，token<10）的条件步成立档缺省 → 继续下一步
                if (n < 10 && (s.type == T_COND_REGION || s.type == T_COND_TOGGLE)) s.a4 = C_CONT;
                if (s.type < 1 || s.type > 23) s.raw = line;   // v6B：12..19 结构行 + 20 终止（已知）；v6D：21 提示；v6E：22 框区域；v6F：23 框矩形；>23 = 未知 raw 保留
                return s;
            } catch (Exception e) {
                return null;
            }
        }

        /** v1..v4 → v5 翻译（对齐面板 ops_translate_old）：计算步定名/索引 + 字段 -6..-9 重映射。 */
        private void translateOld() {
            for (Op o : ops) {
                String[] names = new String[VAR_N];
                int nv = 0;
                for (Step s : o.steps) {                       // 1) 计算步：定名 + 分配索引（按步序；同名复用）
                    if (s.raw != null || s.type != T_CALC) continue;
                    String vn = !s.ref.isEmpty() ? s.ref
                            : ((s.a1 >= 1 && s.a1 <= 4) ? ("r" + s.a1) : null);
                    if (vn == null) continue;                  // 既无名字又无合法槽号：留给核心拒
                    int k = 0;
                    for (; k < nv; k++) if (names[k].equals(vn)) break;
                    if (k == nv) {
                        if (nv >= VAR_N) continue;             // 表满：留给核心拒
                        names[nv] = vn;
                        nv++;
                    }
                    s.a1 = k;
                    s.ref = vn;
                }
                for (Step s : o.steps) {                       // 2) 字段 -6..-9：有对应 r<k> 变量 → 重映射
                    if (s.raw != null) continue;
                    for (int c = 0; c < 5; c++) {              // a1..a4 + ms（j1/j2 不参与翻译）
                        int v = getField(s, c);
                        if (v >= -9 && v <= -6 && varOk(s.type, c)) {
                            String rn = "r" + (-v - 5);
                            for (int k = 0; k < nv; k++) {
                                if (names[k].equals(rn)) { setField(s, c, customCode(k)); break; }
                            }
                        }
                    }
                }
            }
            version = 5;
            header = "#vtouch-ops v5";
        }

        private static int getField(Step s, int c) {
            switch (c) {
                case 0: return s.a1; case 1: return s.a2; case 2: return s.a3;
                case 3: return s.a4; default: return s.ms;
            }
        }

        private static void setField(Step s, int c, int v) {
            switch (c) {
                case 0: s.a1 = v; break; case 1: s.a2 = v; break; case 2: s.a3 = v; break;
                case 3: s.a4 = v; break; default: s.ms = v; break;
            }
        }

        /** 允许变量引用的字段（对齐面板 ope_var_ok；c：0..3 = a1..a4、4 = ms）。 */
        private static boolean varOk(int type, int c) {
            if (c == 4) return type == T_TAP || type == T_SWIPE || type == T_WAIT;
            if (c >= 0 && c <= 3) return type == T_TAP || type == T_SWIPE || type == T_DOWN || type == T_COND_REGION;
            return false;
        }

        public String serialize() {
            StringBuilder sb = new StringBuilder();
            sb.append(version > 5 && header != null ? header : "#vtouch-ops v5").append('\n');
            for (Op o : ops) {
                sb.append("op ").append(o.name).append(" gate ").append(o.gate)
                  .append(" autooff ").append(o.autooff);
                if (o.extra != null) sb.append(' ').append(o.extra);
                sb.append('\n');
                for (Step s : o.steps) {
                    if (s.raw != null) { sb.append(s.raw).append('\n'); continue; }
                    sb.append("step ").append(s.type).append(' ')
                      .append(s.a1).append(' ').append(s.a2).append(' ').append(s.a3).append(' ').append(s.a4).append(' ')
                      .append(s.ms).append(' ')
                      .append(s.ref.isEmpty() ? "-" : s.ref).append(' ')
                      .append(s.j1).append(' ').append(s.j2).append(' ')
                      .append(s.expr.isEmpty() ? "-" : s.expr).append('\n');
                }
            }
            for (String u : unknown) sb.append(u).append('\n');
            return sb.toString();
        }
    }

    // ---------------- 校验（镜像核心 op_valid） ----------------

    /** 允许变量的数值字段：字面值 v ∈ [lo,hi] 或负数编码 -25..-1。 */
    private static boolean numOk(int v, int lo, int hi) {
        return (v >= lo && v <= hi) || (v >= V_CUSTOM15 && v <= V_TDX);
    }

    /** 坐标字段：0..max（max<0 = 屏帧未知，仅收非负）+ 负数编码。 */
    private static boolean coordOk(int v, int max) {
        if (v >= 0 && (max < 0 || v <= max)) return true;
        return v >= V_CUSTOM15 && v <= V_TDX;
    }

    /** v6D：提示模板校验（镜像核心 op_toast_tpl_bad；返回 null 或 `模板错: …`）。 */
    public static String toastTplErr(String tpl, List<String> names) {
        if (tpl == null || tpl.isEmpty()) return "模板错: 模板为空";
        if (tpl.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > EXPR_MAX)
            return "模板错: 超长（≤ " + EXPR_MAX + " 字节）";
        int nph = 0, i = 0, len = tpl.length();
        while (i < len) {
            char ch = tpl.charAt(i);
            if (ch == '{') {
                if (i + 1 < len && tpl.charAt(i + 1) == '{') { i += 2; continue; }
                int j = tpl.indexOf('}', i + 1);
                if (j < 0) return "模板错: 未闭合的 {";
                String nm = tpl.substring(i + 1, j);
                if (nm.isEmpty() || nm.getBytes(java.nio.charset.StandardCharsets.UTF_8).length >= 48)
                    return "模板错: 占位名为空或过长";
                boolean hit = false;
                String[] pre = {"tdx", "tdy", "tux", "tuy", "tms", "fx", "fy"};
                for (String p : pre) if (p.equals(nm)) { hit = true; break; }
                if (!hit && names != null) for (String p : names) if (nm.equals(p)) { hit = true; break; }
                if (!hit) return "模板错: 未知变量 " + nm;
                if (++nph > 16) return "模板错: 占位超限（≤ 16）";
                i = j + 1;
            } else if (ch == '}' && i + 1 < len && tpl.charAt(i + 1) == '}') {
                i += 2;
            } else i++;
        }
        return null;
    }

    /** 只校验操作名（新建草稿用）：步数/步骤字段留到 [完成] / [保存] 时由 validate 把关。
     *  核心要求操作 1..512 步——创建入口不能拿步数拦人（对话框里没处加步骤），否则死胡同。 */
    public static String validateName(Op o, File f, Op self) {
        if (!Regions.idOk(o.name)) return "操作名非法：1..15 个可见字符（可用中文），不能是裸 \"-\"";
        for (Op x : f.ops)
            if (x != self && x.name.equals(o.name)) return "操作名已存在：" + o.name;
        return null;
    }

    /** 校验一条操作（保存前）；self = 被编辑的原对象（新建传 null）。通过返回 null。 */
    public static String validate(Op o, File f, Op self) {
        String ne = validateName(o, f, self);
        if (ne != null) return ne;
        if (!"-".equals(o.gate) && !Regions.idOk(o.gate)) return "门控区域名非法";
        if (o.autooff != 0 && o.autooff != 1) return "autooff 只能是 0 / 1";
        int n = o.steps.size();
        if (n < 1) return "至少要有 1 步";
        if (n > MAX_STEPS) return "步骤数超过 " + MAX_STEPS;
        String[] vars = o.varSlots();
        for (int i = 0; i < VAR_N; i++) {                    // 变量表：名字合法 + 不重名
            String vn = vars[i];
            if (vn == null) continue;
            if (!varNameOk(vn)) return "变量名非法（1..15 个可见字符）：" + vn;
            for (int k = 0; k < i; k++)
                if (vars[k] != null && vars[k].equals(vn)) return "变量重名：" + vn;
        }
        int cxMax = Regions.lastFrameW > 0 ? Regions.lastFrameW - 1 : -1;
        int cyMax = Regions.lastFrameH > 0 ? Regions.lastFrameH - 1 : -1;
        // ---- 控制流结构扫描（v12；镜像核心 op_valid §3.4；v26 抽取为 structureScanErr，供移动落点预检复用） ----
        int[] parent = new int[n];
        String se0 = structureScanErr(o.steps, parent);
        if (se0 != null) return se0;
        for (int i = 0; i < n; i++) {
            Step s = o.steps.get(i);
            if (s.raw != null) continue;                     // 未知逐字保留（原样传；核心会拒）
            if (s.type < 1 || s.type > 23) return "第 " + (i + 1) + " 步类型未知";
            if (!s.expr.isEmpty() && s.type != T_CALC && s.type != T_FINDIMAGE && s.type != T_FINDCOLOR
                    && s.type != T_LOOP && s.type != T_IF && s.type != T_CLAUSE && s.type != T_TOAST)
                return "第 " + (i + 1) + " 步表达式错（该类型不带 expr）";
            String e;
            switch (s.type) {
                case T_TAP:
                    if (!coordOk(s.a1, cxMax) || !coordOk(s.a2, cyMax)) return "第 " + (i + 1) + " 步坐标越界";
                    if (!numOk(s.ms, 0, 60000)) return "第 " + (i + 1) + " 步时长越界";
                    break;
                case T_SWIPE:
                    if (!coordOk(s.a1, cxMax) || !coordOk(s.a2, cyMax)
                            || !coordOk(s.a3, cxMax) || !coordOk(s.a4, cyMax))
                        return "第 " + (i + 1) + " 步坐标越界";
                    if (!numOk(s.ms, 1, 60000)) return "第 " + (i + 1) + " 步时长越界";
                    if (!numOk(s.j1, 0, 60000) || !numOk(s.j2, 0, 60000))
                        return "第 " + (i + 1) + " 步停顿越界（0..60000 或变量）";
                    break;
                case T_WAIT:
                    if (!numOk(s.ms, 0, 600000)) return "第 " + (i + 1) + " 步时长越界";
                    break;
                case T_DOWN:
                    if (!coordOk(s.a1, cxMax) || !coordOk(s.a2, cyMax)) return "第 " + (i + 1) + " 步坐标越界";
                    break;
                case T_UP:
                    break;
                case T_COND_REGION:
                    if (!coordOk(s.a1, cxMax) || !coordOk(s.a2, cyMax)) return "第 " + (i + 1) + " 步坐标越界";
                    if ((e = tierErr(i, s, n)) != null) return e;
                    if (!Regions.idOk(s.ref)) return "第 " + (i + 1) + " 步区域引用非法（1..15 个可见字符）";
                    break;
                case T_COND_TOGGLE:
                    if ((e = tierErr(i, s, n)) != null) return e;
                    if (!Regions.idOk(s.ref)) return "第 " + (i + 1) + " 步区域引用非法（1..15 个可见字符）";
                    break;
                case T_JUMP:
                    if (s.a1 < 0 || s.a1 > n) return "第 " + (i + 1) + " 步跳转目标越界（0.." + n + "）";
                    break;
                case T_CALC: {
                    if (s.a1 < 0 || s.a1 >= VAR_N) return "第 " + (i + 1) + " 步变量索引非法（0.." + (VAR_N - 1) + "）";
                    if (s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0)
                        return "第 " + (i + 1) + " 步字段必须为 0";
                    if (!varNameOk(s.ref)) return "第 " + (i + 1) + " 步变量名非法（1..15 个可见字符）";
                    if (!s.ref.equals(vars[s.a1])) return "第 " + (i + 1) + " 步变量名与名字表不符";
                    if (s.expr.isEmpty()) return "第 " + (i + 1) + " 步表达式为空";
                    String why = Expr.check(s.expr, o.varNames());
                    if (why != null) return "第 " + (i + 1) + " 步表达式错：" + why;
                    break;
                }
                case T_TOAST: {
                    if (s.a1 != 0 || s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty())
                        return "第 " + (i + 1) + " 步字段必须为 0";
                    if (s.ms < 0 || s.ms > 60000) return "第 " + (i + 1) + " 步时长越界（0..60000）";
                    String te = toastTplErr(s.expr, o.varNames());
                    if (te != null) return "第 " + (i + 1) + " 步" + te;
                    break;
                }
                case T_HL: {
                    if (s.a1 != 0 || s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.j1 != 0 || s.j2 != 0)
                        return "第 " + (i + 1) + " 步字段必须为 0";
                    if (s.ms < 0 || s.ms > 60000) return "第 " + (i + 1) + " 步时长越界（0..60000）";
                    if (!Regions.idOk(s.ref)) return "第 " + (i + 1) + " 步区域引用非法（1..15 个可见字符）";
                    break;
                }
                case T_BOX: {
                    if (!coordOk(s.a1, cxMax) || !coordOk(s.a2, cyMax)
                            || !coordOk(s.a3, cxMax) || !coordOk(s.a4, cyMax))
                        return "第 " + (i + 1) + " 步坐标越界";
                    if (!numOk(s.ms, 0, 60000)) return "第 " + (i + 1) + " 步时长越界（0..60000）";
                    if (s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty()) return "第 " + (i + 1) + " 步字段必须为 0";
                    break;
                }
                case T_FINDIMAGE:
                    if (s.a1 < 0 || s.a1 > 255) return "第 " + (i + 1) + " 步阈值越界（0..255）";
                    if (s.a2 != 0 && s.a2 != 1) return "第 " + (i + 1) + " 步字段必须为 0/1";
                    if (!numOk(s.ms, 0, 60000)) return "第 " + (i + 1) + " 步超时越界（0..60000）";
                    if ((e = tierErr(i, s, n)) != null) return e;
                    if (!Regions.idOk(s.ref)) return "第 " + (i + 1) + " 步模板引用非法（1..15 个可见字符）";
                    if (!s.expr.isEmpty() && !Regions.idOk(s.expr))
                        return "第 " + (i + 1) + " 步区域引用非法（1..15 个可见字符）";
                    break;
                case T_FINDCOLOR:
                    if (s.a1 != 0 && s.a1 != 1) return "第 " + (i + 1) + " 步模式非法（0/1）";
                    if (!numOk(s.ms, 0, 60000)) return "第 " + (i + 1) + " 步超时越界（0..60000）";
                    if ((e = tierErr(i, s, n)) != null) return e;
                    if (s.a1 == 1) {
                        if (!Regions.idOk(s.ref)) return "第 " + (i + 1) + " 步点集引用非法（多点必填；1..15 个可见字符）";
                        if (s.a2 != 0) return "第 " + (i + 1) + " 步字段必须为 0";
                    } else {
                        if (!s.ref.isEmpty()) return "第 " + (i + 1) + " 步点集引用必须为空（单点）";
                    }
                    if (!s.expr.isEmpty() && !Regions.idOk(s.expr))
                        return "第 " + (i + 1) + " 步区域引用非法（1..15 个可见字符）";
                    break;
                case T_LOOP: {
                    if (s.a1 < 0 || s.a1 > 3) return "第 " + (i + 1) + " 步循环模式非法（0..3）";
                    if (s.a1 == 0) {
                        if (s.a2 < 1 || s.a2 > 99999) return "第 " + (i + 1) + " 步循环次数越界（1..99999）";
                        if (s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty() || !s.expr.isEmpty())
                            return "第 " + (i + 1) + " 步字段必须为 0";
                    } else if (s.a1 == 1) {
                        if (s.a2 < 1000 || s.a2 > 86400000) return "第 " + (i + 1) + " 步循环时长越界（1000..86400000 ms）";
                        if (s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty() || !s.expr.isEmpty())
                            return "第 " + (i + 1) + " 步字段必须为 0";
                    } else if (s.a1 == 2) {
                        if (s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty() || !s.expr.isEmpty())
                            return "第 " + (i + 1) + " 步字段必须为 0";
                    } else {                            // 到条件（v6B）：j2=取反 0/1
                        if (s.j2 != 0 && s.j2 != 1) return "第 " + (i + 1) + " 步字段必须为 0/1";
                        String c = condErr(i + 1, s.a2, s.a3, s.a4, s.ms, s.ref, s.expr, s.j1, 0, cxMax, cyMax);
                        if (c != null) return c;
                    }
                    break;
                }
                case T_ENDLOOP: case T_ELSE: case T_ENDIF:
                    if (s.a1 != 0 || s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty())
                        return "第 " + (i + 1) + " 步字段必须为 0";
                    break;
                case T_BREAK: case T_CONT:             // v6B：a1=层数（结构扫描已验域）；余字段 0
                    if (s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty())
                        return "第 " + (i + 1) + " 步字段必须为 0";
                    break;
                case T_CLAUSE: {                       // v6B：a1=连接、j2=取反、判定参数（位置/上限已由结构扫描验）
                    if (s.a1 != 0 && s.a1 != 1) return "第 " + (i + 1) + " 步字段必须为 0/1";
                    if (s.j2 != 0 && s.j2 != 1) return "第 " + (i + 1) + " 步字段必须为 0/1";
                    String c = condErr(i + 1, s.a2, s.a3, s.a4, s.ms, s.ref, s.expr, s.j1, 0, cxMax, cyMax);
                    if (c != null) return c;
                    break;
                }
                case T_STOP: {                         // v6B：a1=模式（0 完成收工 / 1 中止退出）；余字段 0
                    if (s.a1 < 0 || s.a1 > 1) return "第 " + (i + 1) + " 步字段必须为 0/1";
                    if (s.a2 != 0 || s.a3 != 0 || s.a4 != 0 || s.ms != 0 || s.j1 != 0 || s.j2 != 0 || !s.ref.isEmpty())
                        return "第 " + (i + 1) + " 步字段必须为 0";
                    break;
                }
                case T_IF: {
                    if (s.a4 != 0 && s.a4 != 1) return "第 " + (i + 1) + " 步字段必须为 0/1";   // v6B：a4=取反
                    String c = condErr(i + 1, s.a1, s.a2, s.a3, s.ms, s.ref, s.expr, s.j1, 0, cxMax, cyMax);
                    if (c != null) return c;
                    break;
                }
                default:
                    return "第 " + (i + 1) + " 步类型未知";
            }
        }
        // ---- 跨块跳校验（v12；镜像核心 §3.4；v26 抽取为 crossBlockErr，供移动落点预检复用） ----
        String ce0 = crossBlockErr(o.steps, parent);
        if (ce0 != null) return ce0;
        return null;
    }

    private static String tierErr(int i, Step s, int n) {
        if (s.a3 < 0 || s.a3 > 3 || s.a4 < 0 || s.a4 > 3)
            return "第 " + (i + 1) + " 步条件档位非法";
        if (s.a4 == C_JUMP && (s.j1 < 0 || s.j1 > n))
            return "第 " + (i + 1) + " 步成立侧跳转目标越界（0.." + n + "）";
        if (s.a3 == C_JUMP && (s.j2 < 0 || s.j2 > n))
            return "第 " + (i + 1) + " 步不成立侧跳转目标越界（0.." + n + "）";
        return null;
    }

    /** 控制流判定参数校验（v12；镜像核心 op_ctrl_cond_bad）：no = 1 基步号；jt：0 区域 / 1 开关 / 2 找图 / 3 找色；
     *  p1,p2 = 判定点或阈值/色校或模式/保留；kms = 找图/找色持续查找；pack = 找色单点打包；extra0 = 该判定保留字段。 */
    private static String condErr(int no, int jt, int p1, int p2, int kms, String ref, String region,
                                  int pack, int extra0, int cxMax, int cyMax) {
        if (jt < 0 || jt > 4) return "第 " + no + " 步判定类型非法（0..4）";
        if (extra0 != 0) return "第 " + no + " 步字段必须为 0";
        switch (jt) {
            case 0:
                if (!coordOk(p1, cxMax) || !coordOk(p2, cyMax)) return "第 " + no + " 步坐标越界";
                if (kms != 0 || pack != 0 || !region.isEmpty()) return "第 " + no + " 步字段必须为 0";
                if (!Regions.idOk(ref)) return "第 " + no + " 步区域引用非法（1..15 个可见字符）";
                break;
            case 1:
                if (p1 != 0 || p2 != 0 || kms != 0 || pack != 0 || !region.isEmpty())
                    return "第 " + no + " 步字段必须为 0";
                if (!Regions.idOk(ref)) return "第 " + no + " 步区域引用非法（1..15 个可见字符）";
                break;
            case 2:
                if (p1 < 0 || p1 > 255) return "第 " + no + " 步阈值越界（0..255）";
                if (p2 != 0 && p2 != 1) return "第 " + no + " 步字段必须为 0/1";
                if (kms < 0 || kms > 60000) return "第 " + no + " 步超时越界（0..60000）";
                if (pack != 0) return "第 " + no + " 步字段必须为 0";
                if (!Regions.idOk(ref)) return "第 " + no + " 步模板引用非法（1..15 个可见字符）";
                if (!region.isEmpty() && !Regions.idOk(region)) return "第 " + no + " 步区域引用非法（1..15 个可见字符）";
                break;
            case 3:
                if (p1 != 0 && p1 != 1) return "第 " + no + " 步模式非法（0/1）";
                if (p2 != 0) return "第 " + no + " 步字段必须为 0";
                if (kms < 0 || kms > 60000) return "第 " + no + " 步超时越界（0..60000）";
                if (p1 == 1) {
                    if (!Regions.idOk(ref)) return "第 " + no + " 步点集引用非法（多点必填；1..15 个可见字符）";
                    if (pack != 0) return "第 " + no + " 步字段必须为 0";
                } else if (!ref.isEmpty()) {
                    return "第 " + no + " 步点集引用必须为空（单点）";
                }
                if (!region.isEmpty() && !Regions.idOk(region)) return "第 " + no + " 步区域引用非法（1..15 个可见字符）";
                break;
            default:                             // case 4 变量比较（v6C）
                if (p1 > -1 || p1 < -25) return "第 " + no + " 步变量编码非法（-25..-1）";
                if (pack < 0 || pack > 5) return "第 " + no + " 步比较符非法（0..5）";
                if (kms != 0 || !ref.isEmpty() || !region.isEmpty()) return "第 " + no + " 步字段必须为 0";
                break;
        }
        return null;
    }

    /** 变量槽名表（静态版：从步表派生；镜像 Op.varSlots()，v6C）。 */
    public static String[] varSlotsOf(List<Step> steps) {
        String[] v = new String[VAR_N];
        for (Step s : steps) {
            if (s.raw == null && s.type == T_CALC && s.a1 >= 0 && s.a1 < VAR_N && !s.ref.isEmpty()) {
                v[s.a1] = s.ref;
            }
        }
        return v;
    }

    // ---------------- v26：结构完好性检查（校验与移动落点预检共用） ----------------

    /** 控制流结构扫描（镜像核心 op_valid §3.4）：块配对 / 嵌套深度 / 17-18 归属 / 子条件链 + 每步直接父块
     *  （写入 parentOut，可空）。返回第一条错误文案或 null。v26 从 validate 抽出——文案勿改（校验提示与测试依赖）。 */
    public static String structureScanErr(List<Step> steps, int[] parentOut) {
        int n = steps.size();
        int[] cstk = new int[8];              // 块头下标栈（12/14 混栈）
        boolean[] cstkIf = new boolean[8];    // true = 条件块头（14）
        boolean[] cstkElse = new boolean[8];  // 条件块是否已出现 15
        int ctop = 0;
        int clauses = 0;                      // v6B：当前块头后的子条件链计数（含头子句；0 = 无链打开）
        for (int i = 0; i < n; i++) {
            Step s = steps.get(i);
            if (parentOut != null) parentOut[i] = ctop > 0 ? cstk[ctop - 1] : -1;
            // v6B 子条件链（镜像核心）：LOOP/IF 开链；19 续链（须紧跟 头 / 12 模式3 / 19）+ 上限；其余断链
            if (s.type == T_LOOP || s.type == T_IF) {
                clauses = 1;
            } else if (s.type == T_CLAUSE) {
                Step pv = i > 0 ? steps.get(i - 1) : null;
                if (clauses == 0 || pv == null || (pv.type == T_LOOP && pv.a1 != 3))
                    return "第 " + (i + 1) + " 步子条件位置错误（须紧跟条件块头 / 循环·到条件 / 上一子条件）";
                if (++clauses > CLAUSE_MAX)
                    return "第 " + (i + 1) + " 步子条件超限（每块头最多 " + CLAUSE_MAX + " 条，含头子句）";
            } else {
                clauses = 0;
            }
            if (s.type == T_LOOP || s.type == T_IF) {
                if (ctop >= 8) return "第 " + (i + 1) + " 步嵌套超深（>8 层）";
                cstk[ctop] = i; cstkIf[ctop] = (s.type == T_IF); cstkElse[ctop] = false; ctop++;
            } else if (s.type == T_ELSE) {
                if (ctop < 1 || !cstkIf[ctop - 1] || cstkElse[ctop - 1])
                    return "第 " + (i + 1) + " 步块配对错误（否则 无对应条件块）";
                cstkElse[ctop - 1] = true;
            } else if (s.type == T_ENDIF) {
                if (ctop < 1 || !cstkIf[ctop - 1])
                    return "第 " + (i + 1) + " 步块配对错误（条件块结束 无对应开始）";
                ctop--;
            } else if (s.type == T_ENDLOOP) {
                if (ctop < 1 || cstkIf[ctop - 1])
                    return "第 " + (i + 1) + " 步块配对错误（循环结束 无对应开始）";
                ctop--;
            } else if (s.type == T_BREAK || s.type == T_CONT) {
                int nl = 0;
                for (int k = 0; k < ctop; k++) if (!cstkIf[k]) nl++;   // 循环层数（条件块不计数）
                if (nl < 1) return "第 " + (i + 1) + " 步跳出不在循环内";
                if (s.a1 < 0 || s.a1 > nl)
                    return "第 " + (i + 1) + " 步跳出层数非法（1..当前循环嵌套深度）";   // v6B §3.3
            }
        }
        if (ctop > 0) {
            if (cstkIf[ctop - 1]) return "第 " + (cstk[ctop - 1] + 1) + " 步块配对错误（条件块未闭合）";
            return "第 " + (cstk[ctop - 1] + 1) + " 步循环未闭合";
        }
        return null;
    }

    /** 跨块跳校验（含结构行时）：跳转/跳过不得跨越块边界、不得落在结构行上。parent = structureScanErr 填的父块表（可空）。
     *  v26 从 validate 抽出；越界目标防御跳过（validate 路径已由字段域校验先行把关，行为不变）。 */
    public static String crossBlockErr(List<Step> steps, int[] parent) {
        int n = steps.size();
        boolean hasBlocks = false;
        for (Step s : steps) if (s.type >= T_LOOP && s.type <= T_CLAUSE) { hasBlocks = true; break; }
        if (!hasBlocks) return null;
        for (int i = 0; i < n; i++) {
            Step s = steps.get(i);
            if (s.raw != null) continue;
            int t1 = -1, t2 = -1;
            if (s.type == T_JUMP) {
                if (s.a1 != 0) t1 = s.a1 - 1;
            } else if (s.type == T_COND_REGION || s.type == T_COND_TOGGLE
                    || s.type == T_FINDIMAGE || s.type == T_FINDCOLOR) {
                if (s.a4 == C_JUMP && s.j1 != 0) t1 = s.j1 - 1;
                if (s.a3 == C_JUMP && s.j2 != 0) t2 = s.j2 - 1;
                if (i + 1 < n && (s.a3 == C_SKIP || s.a4 == C_SKIP)) {
                    int nt = steps.get(i + 1).type;
                    if (nt >= T_LOOP && nt <= T_CLAUSE)
                        return "第 " + (i + 1) + " 步跳转跨越块边界（跳过结构行）";
                }
            } else {
                continue;
            }
            int[] tg = { t1, t2 };
            for (int q = 0; q < 2; q++) {
                if (tg[q] < 0 || tg[q] >= n) continue;
                int dt = steps.get(tg[q]).type;
                if (dt >= T_LOOP && dt <= T_CLAUSE) return "第 " + (i + 1) + " 步跳转跨越块边界（目标是结构行）";
                if (parent != null && parent[tg[q]] != parent[i]) return "第 " + (i + 1) + " 步跳转跨越块边界";
            }
        }
        return null;
    }

    /** 结构完好性总检（扫描 + 跨块跳）：返回第一条错误或 null。v26：validate 与移动落点预检共用。 */
    public static String structureErr(List<Step> steps) {
        int[] parent = new int[steps.size()];
        String e = structureScanErr(steps, parent);
        if (e != null) return e;
        return crossBlockErr(steps, parent);
    }

    // ---------------- 块结构导航 / 分支插入（v12.2：条件块 成立/否则 各自 [＋]） ----------------

    /** v26：移动单元尾下标（块头 12/14 = 配对尾，整块一起移动；其余 = 自身；未配对块头 = -1）。 */
    public static int unitTail(List<Step> steps, int idx) {
        if (idx < 0 || idx >= steps.size()) return -1;
        int t = steps.get(idx).type;
        if (t == T_LOOP || t == T_IF) return pairTail(steps, idx);
        return idx;
    }

    /** 块配对尾下标（head 为 12/14 的块头）：栈法找匹配的 13 / 16；找不到 = -1。 */
    public static int pairTail(List<Step> steps, int head) {
        int d = 0;
        for (int i = head; i < steps.size(); i++) {
            int t = steps.get(i).type;
            if (t == T_LOOP || t == T_IF) d++;
            else if (t == T_ENDLOOP || t == T_ENDIF) {
                d--;
                if (d == 0) return i;
            }
        }
        return -1;
    }

    /** 条件块（head 为 14）「成立分支末尾」：本层「否则」下标；无否则 = 配对尾（16）。找不到 = -1。 */
    public static int ifTrueTail(List<Step> steps, int head) {
        int d = 0;
        for (int i = head + 1; i < steps.size(); i++) {
            int t = steps.get(i).type;
            if (t == T_LOOP || t == T_IF) d++;
            else if (t == T_ENDLOOP || t == T_ENDIF) {
                if (d == 0) return i;
                d--;
            } else if (t == T_ELSE && d == 0) {
                return i;
            }
        }
        return -1;
    }

    /** idx 所在块的头下标（最近未配对 12/14）；顶层 = -1。 */
    public static int blockHeadOf(List<Step> steps, int idx) {
        int d = 0;
        for (int i = idx - 1; i >= 0; i--) {
            int t = steps.get(i).type;
            if (t == T_ENDLOOP || t == T_ENDIF) d++;
            else if (t == T_LOOP || t == T_IF) {
                if (d == 0) return i;
                d--;
            }
        }
        return -1;
    }

    /** 在 at（越界则末尾）插入一步：结构头自动带出配套（12→+13；14→+15+16）；超上限返回 false（不插入）。 */
    public static boolean insertStep(List<Step> steps, int at, Step se) {
        int extra = se.type == T_LOOP ? 1 : (se.type == T_IF ? 2 : 0);
        if (steps.size() + 1 + extra > MAX_STEPS) return false;   // +1 = 插入行本身（v12.2.1 修：原式漏自身，513 会漏过）
        if (at < 0 || at > steps.size()) at = steps.size();
        steps.add(at, se);
        if (se.type == T_LOOP) {
            Step e = new Step();
            e.type = T_ENDLOOP;
            steps.add(at + 1, e);
        } else if (se.type == T_IF) {
            Step e = new Step();
            e.type = T_ELSE;
            Step f = new Step();
            f.type = T_ENDIF;
            steps.add(at + 1, e);
            steps.add(at + 2, f);
        }
        return true;
    }

    // ---------------- v6B：子条件链导航 / 组替换（多条件并或） ----------------

    /** 子条件链尾下标（head 为 12/14；无链 = head 自身；非法输入按非链处理）。 */
    public static int clauseTail(List<Step> steps, int head) {
        if (head < 0 || head >= steps.size()) return head;
        int t = steps.get(head).type;
        if (t != T_LOOP && t != T_IF) return head;
        int j = head;
        while (j + 1 < steps.size() && steps.get(j + 1).type == T_CLAUSE) j++;
        return j;
    }

    /** 子条件数（含头子句；head 为 12/14，非法 = 0）。 */
    public static int clauseCount(List<Step> steps, int head) {
        if (head < 0 || head >= steps.size()) return 0;
        int t = steps.get(head).type;
        if (t != T_LOOP && t != T_IF) return 0;
        return clauseTail(steps, head) - head + 1;
    }

    /** 该头行是否可挂子条件（12 模式3 / 14；v6B）。 */
    public static boolean canClause(Step header) {
        return header.type == T_IF || (header.type == T_LOOP && header.a1 == 3);
    }

    /** 组替换（v6B）：把 [head..clauseTail] 区块替换为 header + clauses（保序 splice；其余步对象零改动）。
     *  clauses = 追加子条件（不含头子句；含取反等已就位）；总数（1 + clauses.size()）超 CLAUSE_MAX 或
     *  总步数超 MAX_STEPS → 返回 false（未改动）。 */
    public static boolean setClauseGroup(List<Step> steps, int head, Step header, List<Step> clauses) {
        if (head < 0 || head >= steps.size()) return false;
        if (1 + clauses.size() > CLAUSE_MAX) return false;
        int tail = clauseTail(steps, head);
        int oldC = tail - head;                       // 现链追加条数
        int newC = clauses.size();
        if (steps.size() - oldC + newC > MAX_STEPS) return false;
        steps.set(head, header);
        int i;
        for (i = 0; i < newC && i < oldC; i++) steps.set(head + 1 + i, clauses.get(i));
        for (; i < newC; i++) steps.add(head + 1 + i, clauses.get(i));       // 追加
        for (int k = 0; k < oldC - newC; k++) steps.remove(head + 1 + newC); // 缩减（从链尾删）
        return true;
    }

    static int pi(String s) { return Integer.parseInt(s.trim()); }
}
