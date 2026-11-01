package com.vtouch.editor;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** vt-script 规范打印器（T1.4）：AST → 规范源码文本。规范 §8：
 *  每语句一行、块体缩进 4 空格、操作数空格分隔、命名参数按固定顺序输出。
 *  判据：print(parse(print(parse(s)))) == print(parse(s))（幂等）；规范形态输入逐字复现。
 *  注：注释保留随 T6 编辑器主题（v1 打印丢弃注释，不影响往返判据）。 */
public final class VtPrint {

    public static String print(VtAst.Block file) {
        return print(file, null);
    }

    /** 直通注释（编辑器 [格式化] 用）：line = 源行号（1 基）；text = 含 `#` 的原文。 */
    public static final class Cmt {
        public final int line;
        public final String text;
        public Cmt(int l, String t) { line = l; text = t; }
    }

    private static List<Cmt> cmts;
    private static int cmtIdx;

    /** 规范打印（可选注释直通；注释随属主语句上方保留；块尾注释留在块内——按块 endLine 归位）。 */
    public static String print(VtAst.Block file, List<Cmt> cs) {
        cmts = cs;
        cmtIdx = 0;
        try {
            StringBuilder b = new StringBuilder();
            for (VtAst.St s : file.sts) st(b, s, 0);
            flush(b, Integer.MAX_VALUE, 0);      // 文件尾剩余
            return b.toString();
        } finally {
            cmts = null;
        }
    }

    /** 把「源行号 < beforeLine」的待印注释以 lv 缩进依次打出。 */
    private static void flush(StringBuilder b, int beforeLine, int lv) {
        if (cmts == null) return;
        while (cmtIdx < cmts.size() && cmts.get(cmtIdx).line < beforeLine) {
            ind(b, lv);
            b.append(cmts.get(cmtIdx).text.trim()).append('\n');
            cmtIdx++;
        }
    }

    /** 单表达式规范打印（T6：赋值步 step.expr 的落盘形态）。 */
    public static String printExpr(VtAst.Ex e) {
        StringBuilder b = new StringBuilder();
        ex(b, e, 0);
        return b.toString();
    }

    private static void ind(StringBuilder b, int lv) {
        for (int i = 0; i < lv; i++) b.append("    ");
    }

    /* ---------- 语句 ---------- */
    private static void st(StringBuilder b, VtAst.St s, int lv) {
        flush(b, s.line, lv);
        ind(b, lv);
        if (s instanceof VtAst.ActionSt) {
            VtAst.ActionSt a = (VtAst.ActionSt) s;
            b.append(VtTok.KW_NAMES[a.kw - 1]);
            for (VtAst.Ex e : a.args) { b.append(' '); ex(b, e, 0); }
            opts(b, a.opts, a.kw == VtTok.K_HUADONG ? ORDER_滑动 : null);
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.AssignSt) {
            VtAst.AssignSt a = (VtAst.AssignSt) s;
            b.append(a.name);
            if (a.idx != null) { b.append('['); ex(b, a.idx, 0); b.append(']'); }
            b.append(" = ");
            ex(b, a.val, 0);
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.IfSt) {
            VtAst.IfSt f = (VtAst.IfSt) s;
            b.append("如果 ");
            ex(b, f.cond, 0);
            b.append(" {\n");
            for (VtAst.St x : f.thenB.sts) st(b, x, lv + 1);
            flush(b, f.thenB.endLine > 0 ? f.thenB.endLine : Integer.MAX_VALUE, lv + 1);
            ind(b, lv);
            b.append('}');
            if (f.elseB != null) {
                b.append(" 否则 {\n");
                for (VtAst.St x : f.elseB.sts) st(b, x, lv + 1);
                flush(b, f.elseB.endLine > 0 ? f.elseB.endLine : Integer.MAX_VALUE, lv + 1);
                ind(b, lv);
                b.append('}');
            }
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.LoopSt) {
            VtAst.LoopSt l = (VtAst.LoopSt) s;
            b.append("循环 ");
            if (l.mode == 1) b.append("无限");
            else if (l.mode == 2) {
                boolean first = true;
                for (VtAst.Opt o : ordered(l.opts, ORDER_循环)) {
                    if (!first) b.append(' ');
                    first = false;
                    b.append(o.key).append('=');
                    ex(b, o.val, 0);
                }
            } else if (l.mode == 3) {
                b.append(l.var).append(" 从 ");
                ex(b, l.from, 0);
                b.append(" 到 ");
                ex(b, l.to, 0);
            } else ex(b, l.count, 0);
            b.append(" {\n");
            for (VtAst.St x : l.body.sts) st(b, x, lv + 1);
            flush(b, l.body.endLine > 0 ? l.body.endLine : Integer.MAX_VALUE, lv + 1);
            ind(b, lv);
            b.append("}\n");
            return;
        }
        if (s instanceof VtAst.WhileSt) {
            VtAst.WhileSt w = (VtAst.WhileSt) s;
            b.append(w.neg ? "直到 " : "当 ");
            ex(b, w.cond, 0);
            b.append(" {\n");
            for (VtAst.St x : w.body.sts) st(b, x, lv + 1);
            flush(b, w.body.endLine > 0 ? w.body.endLine : Integer.MAX_VALUE, lv + 1);
            ind(b, lv);
            b.append("}\n");
            return;
        }
        if (s instanceof VtAst.CtlSt) {
            VtAst.CtlSt c = (VtAst.CtlSt) s;
            b.append(VtTok.KW_NAMES[c.kw - 1]);
            if (c.layers != 1) b.append(' ').append(c.layers);
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.TermSt) {
            b.append("终止");
            if (((VtAst.TermSt) s).abort) b.append(" 中止");
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.RetSt) {
            VtAst.RetSt r = (VtAst.RetSt) s;
            b.append("返回");
            if (r.val != null) { b.append(' '); ex(b, r.val, 0); }
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.ToastSt) {
            VtAst.ToastSt t = (VtAst.ToastSt) s;
            b.append("提示 ");
            ex(b, t.msg, 0);
            opts(b, t.opts, ORDER_时长);
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.HlSt) {
            VtAst.HlSt h = (VtAst.HlSt) s;
            b.append("框区域 \"").append(sq(h.region)).append('"');
            opts(b, h.opts, ORDER_时长);
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.BoxSt) {
            VtAst.BoxSt x = (VtAst.BoxSt) s;
            b.append("框矩形 ");
            ex(b, x.x1, 0); b.append(' ');
            ex(b, x.y1, 0); b.append(' ');
            ex(b, x.x2, 0); b.append(' ');
            ex(b, x.y2, 0);
            opts(b, x.opts, ORDER_时长);
            b.append('\n');
            return;
        }
        if (s instanceof VtAst.FnSt) {
            VtAst.FnSt f = (VtAst.FnSt) s;
            b.append("函数 ").append(f.name).append('(');
            for (int i = 0; i < f.params.size(); i++) {
                if (i > 0) b.append(", ");
                b.append(f.params.get(i));
            }
            b.append(") {\n");
            for (VtAst.St x : f.body.sts) st(b, x, lv + 1);
            flush(b, f.body.endLine > 0 ? f.body.endLine : Integer.MAX_VALUE, lv + 1);
            ind(b, lv);
            b.append("}\n");
            return;
        }
        if (s instanceof VtAst.TableSt) {
            VtAst.TableSt t = (VtAst.TableSt) s;
            b.append("表 ").append(t.name).append('[').append(t.len).append("]\n");
            return;
        }
        if (s instanceof VtAst.JmpSt) {
            VtAst.JmpSt j = (VtAst.JmpSt) s;
            if (j.end) b.append("跳转 结束\n");
            else b.append("跳转 第").append(j.target).append("行\n");
            return;
        }
        if (s instanceof VtAst.CallSt) {
            ex(b, ((VtAst.CallSt) s).call, 0);
            b.append('\n');
            return;
        }
        b.append("?\n");
    }

    /* ---------- 命名参数（规范序） ---------- */
    private static final String[] ORDER_滑动 = {"时长", "停1", "停2"};
    private static final String[] ORDER_时长 = {"时长"};
    private static final String[] ORDER_循环 = {"次数", "时长"};

    private static String[] orderOf(String callName) {
        if (callName.equals("找图")) return new String[]{"区域", "阈值", "色校", "持续"};
        if (callName.equals("找色")) return new String[]{"区域"};
        if (callName.equals("找色点")) return new String[]{"容差", "区域"};
        return null;
    }

    private static void opts(StringBuilder b, List<VtAst.Opt> os, String[] order) {
        for (VtAst.Opt o : ordered(os, order)) {
            b.append(' ').append(o.key).append('=');
            ex(b, o.val, 0);
        }
    }

    private static List<VtAst.Opt> ordered(List<VtAst.Opt> os, String[] order) {
        if (os == null || os.size() < 2 || order == null) return os;
        final Map<String, Integer> idx = new HashMap<String, Integer>();
        for (int i = 0; i < order.length; i++) idx.put(order[i], i);
        List<VtAst.Opt> c = new ArrayList<VtAst.Opt>(os);
        Collections.sort(c, new Comparator<VtAst.Opt>() {
            public int compare(VtAst.Opt x, VtAst.Opt y) {
                int ix = idx.containsKey(x.key) ? idx.get(x.key).intValue() : 99;
                int iy = idx.containsKey(y.key) ? idx.get(y.key).intValue() : 99;
                return ix - iy;
            }
        });
        return c;
    }

    /* ---------- 表达式（最小括号） ---------- */
    private static final int P1 = 1, P2 = 2, P3 = 3, P4 = 4, P5 = 5, P6 = 6;

    private static int prec(VtAst.Ex e) {
        if (e instanceof VtAst.BinEx) {
            String o = ((VtAst.BinEx) e).op;
            if (o.equals("||")) return P1;
            if (o.equals("&&")) return P2;
            if (o.equals("==") || o.equals("!=") || o.equals("<") || o.equals(">") || o.equals("<=") || o.equals(">=")) return P3;
            if (o.equals("+") || o.equals("-")) return P4;
            return P5;
        }
        if (e instanceof VtAst.UnEx) return P6;
        return 7;
    }

    private static boolean cmpOp(String o) {
        return o.equals("==") || o.equals("!=") || o.equals("<") || o.equals(">") || o.equals("<=") || o.equals(">=");
    }

    /** 以最小括号打印：子优先级 < ctx → 加括号；左结合右子同级、比较两侧同级 → 加括号。 */
    private static void ex(StringBuilder b, VtAst.Ex e, int ctx) {
        int p = prec(e);
        boolean wrap = p < ctx;
        if (wrap) b.append('(');
        if (e instanceof VtAst.BinEx) {
            VtAst.BinEx x = (VtAst.BinEx) e;
            boolean cmp = cmpOp(x.op);
            ex(b, x.l, cmp ? p + 1 : p);
            b.append(' ').append(x.op).append(' ');
            ex(b, x.r, p + 1);
        } else if (e instanceof VtAst.UnEx) {
            VtAst.UnEx u = (VtAst.UnEx) e;
            b.append(u.op);
            if (u.op.equals("-") && startsMinus(u.e)) b.append(' ');
            ex(b, u.e, P6);
        } else {
            primEx(b, e);
        }
        if (wrap) b.append(')');
    }

    private static boolean startsMinus(VtAst.Ex e) {
        return e instanceof VtAst.UnEx && ((VtAst.UnEx) e).op.equals("-");
    }

    private static void primEx(StringBuilder b, VtAst.Ex e) {
        if (e instanceof VtAst.NumEx) {
            b.append(((VtAst.NumEx) e).raw);
            return;
        }
        if (e instanceof VtAst.StrEx) {
            VtAst.StrEx s = (VtAst.StrEx) e;
            b.append('"').append(s.raw != null ? s.raw : synthStr(s)).append('"');
            return;
        }
        if (e instanceof VtAst.LitEx) {
            b.append(VtTok.KW_NAMES[((VtAst.LitEx) e).kw - 1]);
            return;
        }
        if (e instanceof VtAst.IdEx) {
            b.append(((VtAst.IdEx) e).name);
            return;
        }
        if (e instanceof VtAst.IdxEx) {
            VtAst.IdxEx x = (VtAst.IdxEx) e;
            b.append(x.name).append('[');
            ex(b, x.idx, 0);
            b.append(']');
            return;
        }
        if (e instanceof VtAst.CallEx) {
            VtAst.CallEx c = (VtAst.CallEx) e;
            b.append(c.name).append('(');
            boolean first = true;
            for (VtAst.Ex a : c.args) {
                if (!first) b.append(", ");
                first = false;
                ex(b, a, 0);
            }
            for (VtAst.Opt o : ordered(c.opts, orderOf(c.name))) {
                if (!first) b.append(", ");
                first = false;
                b.append(o.key).append('=');
                ex(b, o.val, 0);
            }
            b.append(')');
            return;
        }
        b.append('?');
    }

    /* ---------- 字符串 ---------- */

    /** 纯文本转义（框区域 id 等）。 */
    private static String sq(String v) {
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

    /** 无 raw 的字符串（程序化构造）→ 由段合成（字面 {} 用 {{ }} 转义）。 */
    private static String synthStr(VtAst.StrEx s) {
        StringBuilder b = new StringBuilder();
        if (s.segs == null) return "";
        for (Object o : s.segs) {
            if (o instanceof String) {
                String t = (String) o;
                for (int i = 0; i < t.length(); i++) {
                    char c = t.charAt(i);
                    if (c == '"') b.append("\\\"");
                    else if (c == '\\') b.append("\\\\");
                    else if (c == '\n') b.append("\\n");
                    else if (c == '\t') b.append("\\t");
                    else if (c == '{') b.append("{{");
                    else if (c == '}') b.append("}}");
                    else b.append(c);
                }
            } else {
                VtTok.Interp ip = (VtTok.Interp) o;
                b.append('{').append(ip.name);
                if (ip.hasIdx) {
                    b.append('[');
                    if (ip.idxNum) b.append((long) ip.idx);
                    else b.append(ip.idxName);
                    b.append(']');
                }
                b.append('}');
            }
        }
        return b.toString();
    }
}
