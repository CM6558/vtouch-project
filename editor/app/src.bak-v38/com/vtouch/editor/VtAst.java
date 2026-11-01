package com.vtouch.editor;

import java.util.List;

/** vt-script AST（T1.2）。规范：docs/VT_SCRIPT_V1_SPEC.md §7。
 *  纯 Java、零 Android 依赖。节点不带语义——名字/类型/上下文校验在 VtCheck（T1.3）。 */
public final class VtAst {

    public static abstract class Node { public int line, col; }

    /* ===== 表达式 ===== */
    public static abstract class Ex extends Node {}

    /** 数字字面量（raw = 原文，打印用）。 */
    public static final class NumEx extends Ex { public double v; public String raw; }
    /** 字符串字面量（segs = String 字面段 | VtTok.Interp）。 */
    public static final class StrEx extends Ex { public String raw; public List<Object> segs; }
    /** 真 / 假 / 轮（kw = VtTok.K_ZHEN / K_JIA / K_LUN）。 */
    public static final class LitEx extends Ex { public int kw; }
    /** 变量 / 表名（语义在 T1.3 解析）。 */
    public static final class IdEx extends Ex { public String name; }
    /** 表读取：名[下标]。 */
    public static final class IdxEx extends Ex { public String name; public Ex idx; }
    /** 调用（用户函数 / 内建 / 视觉）：位置实参 + 命名实参。 */
    public static final class CallEx extends Ex { public String name; public List<Ex> args; public List<Opt> opts; }
    /** 二元：+ - * / % == != < > <= >= && ||。 */
    public static final class BinEx extends Ex { public String op; public Ex l, r; }
    /** 一元：! -。 */
    public static final class UnEx extends Ex { public String op; public Ex e; }

    /** 命名实参（键=值）。 */
    public static final class Opt extends Node { public String key; public Ex val; }

    /* ===== 语句 ===== */
    public static abstract class St extends Node {}
    public static final class Block extends Node { public List<St> sts; public int endLine; /* v36：闭括号行（注释归位用） */ }

    /** 点按/滑动/等待/按下/弹起（kw = VtTok.K_*；args 按语句表）。 */
    public static final class ActionSt extends St { public int kw; public List<Ex> args; public List<Opt> opts; }
    /** 赋值（idx 可空）。 */
    public static final class AssignSt extends St { public String name; public Ex idx; public Ex val; }
    /** 如果 / 否则（elseB 可空）。 */
    public static final class IfSt extends St { public Ex cond; public Block thenB; public Block elseB; }
    /** 循环：mode 0=计数表达式 1=无限 2=选项键（次数=/时长=） 3=for（var 从 from 到 to）。 */
    public static final class LoopSt extends St {
        public int mode;
        public Ex count;
        public List<Opt> opts;
        public String var; public Ex from, to;
        public Block body;
    }
    /** 当 / 直到（neg = 直到）。 */
    public static final class WhileSt extends St { public boolean neg; public Ex cond; public Block body; }
    /** 跳出 / 继续（kw = VtTok.K_TIAOCHU / K_JIXU）。 */
    public static final class CtlSt extends St { public int kw; public int layers; }
    public static final class TermSt extends St { public boolean abort; }
    public static final class RetSt extends St { public Ex val; }
    public static final class ToastSt extends St { public Ex msg; public List<Opt> opts; }
    public static final class HlSt extends St { public String region; public List<Opt> opts; }
    public static final class BoxSt extends St { public Ex x1, y1, x2, y2; public List<Opt> opts; }
    public static final class FnSt extends St { public String name; public List<String> params; public Block body; }
    public static final class TableSt extends St { public String name; public int len; }
    /** 跳转：end = 结束；否则 target = 行号（≥1）。 */
    public static final class JmpSt extends St { public int target; public boolean end; }
    /** 语句位调用（用户函数/内建/视觉）。 */
    public static final class CallSt extends St { public CallEx call; }

    /** 文件根（= 顶层语句列表；无块括号）。 */
    public static VtAst.Block file() {
        VtAst.Block b = new VtAst.Block();
        b.line = 1; b.col = 1;
        b.sts = new java.util.ArrayList<St>();
        return b;
    }
}
