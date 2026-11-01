package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * ops.conf 的模型与读写。
 *
 * 已确定部分（仓库 v3 口径 + 设备实物核对）：
 *   #vtouch-ops v<N>                                （版本行逐字保留；设备当前为 v5）
 *   op   <名> gate <门控区域|-> autooff <0|1>
 *   step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> [第 10 字段起：原样保留]
 * 类型 1..8（点按/滑动/等待/按下/弹起/区域判断/开关判断/跳转）全功能编辑；
 * 未知类型或解析失败的行**逐字保留**（设备版新特性，等最新代码推送后再解锁）。
 * ref 空写 "-"；变量照写负数（-1..-5 已知：触发按下x/按下y/弹起x/弹起y/时长）。
 */
public final class Ops {
    public static final String PATH = "/data/local/vtouch-runtime/ops.conf";

    public static final int T_TAP = 1, T_SWIPE = 2, T_WAIT = 3, T_DOWN = 4, T_UP = 5,
            T_COND_REGION = 6, T_COND_TOGGLE = 7, T_JUMP = 8;
    public static final int C_ABORT = 0, C_SKIP = 1, C_CONT = 2, C_JUMP = 3;

    public static String typeName(int t) {
        switch (t) {
            case 1: return "点按"; case 2: return "滑动"; case 3: return "等待";
            case 4: return "按下"; case 5: return "弹起"; case 6: return "区域判断";
            case 7: return "开关判断"; case 8: return "跳转";
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

    public static final class Step {
        public int type;
        public int a1, a2, a3, a4, ms;
        public String ref = "";
        public int j1, j2;
        public String extra = null;   // 第 10 字段起（原样；null = 原行没有）
        public String raw = null;     // 未知类型/解析失败：原行逐字

        public boolean known() { return raw == null; }

        public Step copy() {
            Step s = new Step();
            s.type = type; s.a1 = a1; s.a2 = a2; s.a3 = a3; s.a4 = a4; s.ms = ms;
            s.ref = ref; s.j1 = j1; s.j2 = j2; s.extra = extra; s.raw = raw;
            return s;
        }

        public String summary() {
            if (raw != null) return "原样保留: " + raw;
            switch (type) {
                case T_TAP: return "点按 " + a1 + "," + a2 + "  " + ms + "ms";
                case T_SWIPE: return "滑动 " + a1 + "," + a2 + " → " + a3 + "," + a4 + "  " + ms + "ms";
                case T_WAIT: return "等待 " + ms + "ms";
                case T_DOWN: return "按下 " + a1 + "," + a2;
                case T_UP: return "弹起";
                case T_COND_REGION:
                    return "区域判断 " + ref + "  成立:" + sideText(a4, j1) + " / 不成立:" + sideText(a3, j2);
                case T_COND_TOGGLE:
                    return "开关判断 " + ref + "  成立:" + sideText(a4, j1) + " / 不成立:" + sideText(a3, j2);
                case T_JUMP: return "跳转 → " + (a1 == 0 ? "结束" : "第 " + a1 + " 步");
                default: return "未知类型" + type;
            }
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
                } else if (t[0].equals("step") && t.length >= 10 && current != null) {
                    Step s = new Step();
                    s.type = pi(t[1]);
                    s.a1 = pi(t[2]); s.a2 = pi(t[3]); s.a3 = pi(t[4]); s.a4 = pi(t[5]);
                    s.ms = pi(t[6]);
                    s.ref = t[7].equals("-") ? "" : t[7];
                    s.j1 = pi(t[8]); s.j2 = pi(t[9]);
                    if (t.length > 10) {
                        StringBuilder sb = new StringBuilder();
                        for (int i = 10; i < t.length; i++) { if (i > 10) sb.append(' '); sb.append(t[i]); }
                        s.extra = sb.toString();
                    }
                    if (s.type < 1 || s.type > 8) s.raw = line;
                    current.steps.add(s);
                } else {
                    f.unknown.add(line);
                }
            }
            if (f.header == null) throw new IllegalArgumentException("操作文件缺少 #vtouch-ops 版本行");
            return f;
        }

        public String serialize() {
            StringBuilder sb = new StringBuilder();
            sb.append(header != null ? header : "#vtouch-ops v5").append('\n');
            boolean writeExtra = version >= 4;   // v4+ 起有第 10 字段；v3 文件保持 9 字段
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
                      .append(s.j1).append(' ').append(s.j2);
                    if (writeExtra || s.extra != null) sb.append(' ').append(s.extra != null ? s.extra : "-");
                    sb.append('\n');
                }
            }
            for (String u : unknown) sb.append(u).append('\n');
            return sb.toString();
        }
    }

    /** 校验一条操作（保存前）；self = 被编辑的原对象（新建传 null）。通过返回 null。 */
    public static String validate(Op o, File f, Op self) {
        if (!Regions.idOk(o.name)) return "操作名非法：字符集 [A-Za-z0-9_-]、长度 1..15、不能是裸 \"-\"";
        for (Op x : f.ops)
            if (x != self && x.name.equals(o.name)) return "操作名已存在：" + o.name;
        if (!"-".equals(o.gate) && !Regions.idOk(o.gate)) return "门控区域名非法";
        if (o.autooff != 0 && o.autooff != 1) return "autooff 只能是 0 / 1";
        int n = o.steps.size();
        if (n > 32) return "步骤数超过 32";
        for (int i = 0; i < n; i++) {
            Step s = o.steps.get(i);
            if (s.raw != null) continue;
            if (s.type < 1 || s.type > 8) return "第 " + (i + 1) + " 步类型未知";
            if (s.type == T_COND_REGION || s.type == T_COND_TOGGLE) {
                if (s.a3 < 0 || s.a3 > 3 || s.a4 < 0 || s.a4 > 3)
                    return "第 " + (i + 1) + " 步条件档位非法";
                if (s.a4 == C_JUMP && (s.j1 < 0 || s.j1 > n))
                    return "第 " + (i + 1) + " 步成立侧跳转目标越界（0.." + n + "）";
                if (s.a3 == C_JUMP && (s.j2 < 0 || s.j2 > n))
                    return "第 " + (i + 1) + " 步不成立侧跳转目标越界（0.." + n + "）";
            }
            if (s.type == T_JUMP && (s.a1 < 0 || s.a1 > n))
                return "第 " + (i + 1) + " 步跳转目标越界（0.." + n + "）";
        }
        return null;
    }

    static int pi(String s) { return Integer.parseInt(s.trim()); }
}
