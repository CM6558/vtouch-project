package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/** VTBC 反汇编器 + 结构自检（T2.3，编辑器侧人查用）。格式见 docs/VTBC_V1.md。
 *  render = 逐指令文本；check = 结构自检（指令流可解码到底 / 跳转与函数入口落指令边界 /
 *  行号表按偏移升序（行号值可不单调——函数体取其定义行）/ 常量·表·函数引用界）。返回 null = 过。深度校验（栈深/语义）在 T3.4 核心验证器。 */
public final class VtDisasm {

    /** 指令步长（字节）；-1 = 未知指令。 */
    public static int step(int op) {
        switch (op) {
            case VtOp.KN: case VtOp.KS: case VtOp.LV: case VtOp.SV: case VtOp.LT:
            case VtOp.ST: case VtOp.JMP: case VtOp.JF: case VtOp.JT: case VtOp.CJMP:
            case VtOp.HL: case VtOp.SWON: case VtOp.INREGION: case VtOp.FINDDOT:
                return 5;
            case VtOp.FINDIMG: case VtOp.FINDCOLOR:
                return 9;
            case VtOp.CALL:
                return 6;
            case VtOp.CMP: case VtOp.TERM:
                return 2;
            case VtOp.ADD: case VtOp.SUB: case VtOp.MUL: case VtOp.DIV: case VtOp.MOD:
            case VtOp.NEG: case VtOp.NOT: case VtOp.POP: case VtOp.RET: case VtOp.RETE:
            case VtOp.TAP: case VtOp.SWIPE: case VtOp.WAIT: case VtOp.DOWN: case VtOp.UP:
            case VtOp.TOAST: case VtOp.BOX: case VtOp.NOW: case VtOp.LEN: case VtOp.STRN:
            case VtOp.NUM: case VtOp.RAND: case VtOp.FLOOR: case VtOp.CEIL: case VtOp.ROUND:
            case VtOp.ABS: case VtOp.MIN: case VtOp.MAX: case VtOp.SQRT: case VtOp.SIN:
            case VtOp.COS: case VtOp.ATAN2:
                return 1;
            default:
                return -1;
        }
    }

    public static String render(VtBc.Blob b) {
        StringBuilder s = new StringBuilder();
        byte[] c = b.code;
        int p = 0;
        while (p < c.length) {
            int off = p;
            int op = c[p++] & 0xFF;
            String t;
            switch (op) {
                case VtOp.KN: { int n = r32(c, p); p += 4; t = "KN #" + n + " " + fmtD(b.consts.get(n).num); break; }
                case VtOp.KS: { int n = r32(c, p); p += 4; t = "KS #" + n + " \"" + b.consts.get(n).str + "\""; break; }
                case VtOp.LV: case VtOp.SV: case VtOp.LT: case VtOp.ST:
                case VtOp.HL: case VtOp.SWON: case VtOp.INREGION: case VtOp.FINDDOT: {
                    int n = r32(c, p); p += 4; t = opName(op) + " #" + n; break;
                }
                case VtOp.JMP: case VtOp.JF: case VtOp.JT: case VtOp.CJMP: {
                    int n = r32(c, p); p += 4; t = opName(op) + " ->" + hex4(n); break;
                }
                case VtOp.CALL: { int n = r32(c, p); int a = c[p + 4] & 0xFF; p += 5; t = "CALL " + n + " " + a; break; }
                case VtOp.FINDIMG: case VtOp.FINDCOLOR: {
                    int a1 = r32(c, p), a2 = r32(c, p + 4); p += 8;
                    t = opName(op) + " #" + a1 + " #" + a2; break;
                }
                case VtOp.CMP: case VtOp.TERM: { int a = c[p++] & 0xFF; t = opName(op) + " " + a; break; }
                default: t = opName(op);
            }
            s.append(hex4(off)).append(": ").append(t).append('\n');
        }
        return s.toString();
    }

    /** 结构自检：null = 过；否则原因（人查/测试用）。 */
    public static String check(VtBc.Blob b) {
        byte[] c = b.code;
        List<Integer> bounds = new ArrayList<Integer>();
        List<Integer> jmpAt = new ArrayList<Integer>();
        List<Integer> jmpTo = new ArrayList<Integer>();
        int p = 0;
        while (p < c.length) {
            bounds.add(Integer.valueOf(p));
            int op = c[p] & 0xFF;
            int st = step(op);
            if (st < 0) return "未知指令 @" + p;
            if (p + st > c.length) return "指令截断 @" + p;
            switch (op) {
                case VtOp.KN: case VtOp.KS: {
                    int n = r32(c, p + 1);
                    if (n < 0 || n >= b.consts.size()) return "常量越界 @" + p;
                    break;
                }
                case VtOp.LT: case VtOp.ST: {
                    int n = r32(c, p + 1);
                    if (n < 0 || n >= b.tables.size()) return "表越界 @" + p;
                    break;
                }
                case VtOp.HL: case VtOp.SWON: case VtOp.INREGION: {
                    int n = r32(c, p + 1);
                    if (n < 0 || n >= b.consts.size() || b.consts.get(n).kind != 1) return "字符串常量越界 @" + p;
                    break;
                }
                case VtOp.FINDDOT: {
                    int n = r32(c, p + 1);
                    if (n != -1 && (n < 0 || n >= b.consts.size() || b.consts.get(n).kind != 1)) return "区域常量越界 @" + p;
                    break;
                }
                case VtOp.FINDIMG: case VtOp.FINDCOLOR: {
                    int a1 = r32(c, p + 1), a2 = r32(c, p + 5);
                    if (a1 < 0 || a1 >= b.consts.size() || b.consts.get(a1).kind != 1) return "常量越界 @" + p;
                    if (a2 != -1 && (a2 < 0 || a2 >= b.consts.size() || b.consts.get(a2).kind != 1)) return "区域常量越界 @" + p;
                    break;
                }
                case VtOp.CALL: {
                    int fi = r32(c, p + 1);
                    if (fi < 0 || fi >= b.fns.size()) return "函数号越界 @" + p;
                    break;
                }
                case VtOp.JMP: case VtOp.JF: case VtOp.JT: case VtOp.CJMP: {
                    jmpAt.add(Integer.valueOf(p));
                    jmpTo.add(Integer.valueOf(r32(c, p + 1)));
                    break;
                }
                default:
                    break;
            }
            p += st;
        }
        if (p != c.length) return "指令流尾部不齐";
        for (int i = 0; i < jmpTo.size(); i++) {
            int t = jmpTo.get(i).intValue();
            if (t != c.length && !bounds.contains(Integer.valueOf(t)))
                return "跳转目标非指令边界 @" + jmpAt.get(i) + " -> " + t;
        }
        for (int i = 0; i < b.fns.size(); i++) {
            VtBc.Fn f = b.fns.get(i);
            if (f.entry < 0 || (f.entry != c.length && !bounds.contains(Integer.valueOf(f.entry))))
                return "函数入口非指令边界 #" + i;
        }
        for (int i = 1; i < b.lines.size(); i++)
            if (b.lines.get(i).off <= b.lines.get(i - 1).off)
                return "行号表乱序 @" + b.lines.get(i).off;
        return null;
    }

    static int r32(byte[] c, int p) {
        return (c[p] & 0xFF) | ((c[p + 1] & 0xFF) << 8) | ((c[p + 2] & 0xFF) << 16) | ((c[p + 3] & 0xFF) << 24);
    }
    static String hex4(int v) { return String.format(java.util.Locale.US, "%04X", v & 0xFFFF); }
    static String fmtD(double d) {
        if (d == Math.rint(d) && Math.abs(d) < 1e15) return String.valueOf((long) d);
        String s = String.format(java.util.Locale.US, "%.6f", d);
        while (s.endsWith("0")) s = s.substring(0, s.length() - 1);
        if (s.endsWith(".")) s = s.substring(0, s.length() - 1);
        return s;
    }
    static String opName(int op) {
        switch (op) {
            case VtOp.KN: return "KN";
            case VtOp.KS: return "KS";
            case VtOp.LV: return "LV";
            case VtOp.SV: return "SV";
            case VtOp.LT: return "LT";
            case VtOp.ST: return "ST";
            case VtOp.ADD: return "ADD";
            case VtOp.SUB: return "SUB";
            case VtOp.MUL: return "MUL";
            case VtOp.DIV: return "DIV";
            case VtOp.MOD: return "MOD";
            case VtOp.NEG: return "NEG";
            case VtOp.NOT: return "NOT";
            case VtOp.CMP: return "CMP";
            case VtOp.POP: return "POP";
            case VtOp.JMP: return "JMP";
            case VtOp.JF: return "JF";
            case VtOp.JT: return "JT";
            case VtOp.CJMP: return "CJMP";
            case VtOp.CALL: return "CALL";
            case VtOp.RET: return "RET";
            case VtOp.RETE: return "RETE";
            case VtOp.TAP: return "TAP";
            case VtOp.SWIPE: return "SWIPE";
            case VtOp.WAIT: return "WAIT";
            case VtOp.DOWN: return "DOWN";
            case VtOp.UP: return "UP";
            case VtOp.TOAST: return "TOAST";
            case VtOp.HL: return "HL";
            case VtOp.BOX: return "BOX";
            case VtOp.FINDIMG: return "FINDIMG";
            case VtOp.FINDCOLOR: return "FINDCOLOR";
            case VtOp.FINDDOT: return "FINDDOT";
            case VtOp.INREGION: return "INREGION";
            case VtOp.SWON: return "SWON";
            case VtOp.TERM: return "TERM";
            case VtOp.NOW: return "NOW";
            case VtOp.LEN: return "LEN";
            case VtOp.STRN: return "STRN";
            case VtOp.NUM: return "NUM";
            case VtOp.RAND: return "RAND";
            case VtOp.FLOOR: return "FLOOR";
            case VtOp.CEIL: return "CEIL";
            case VtOp.ROUND: return "ROUND";
            case VtOp.ABS: return "ABS";
            case VtOp.MIN: return "MIN";
            case VtOp.MAX: return "MAX";
            case VtOp.SQRT: return "SQRT";
            case VtOp.SIN: return "SIN";
            case VtOp.COS: return "COS";
            case VtOp.ATAN2: return "ATAN2";
            default: return "??" + Integer.toHexString(op);
        }
    }
}
