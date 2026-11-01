package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * regions.conf（v2）的模型与读写。
 *
 * 格式以核心 src/vt_region.c（region_add / region_bind / region_kind_set 的字段语义）
 * 与面板 save_regions 的写出格式为准：
 *   #vtouch-regions v2
 *   #frame <rot> <w> <h>            （这些数字所属的屏帧；原样保留，不解释）
 *   region <id> <type> <a1> <a2> <a3> <a4> <en>     type 0=矩形(x1 y1 x2 y2) 1=圆(cx cy r a4未用)
 *   bind   <id> <操作名|-> <down|press>              未绑定写 "-" + "down"
 *   kind   <id> <0|1>                                0=普通 1=开关型
 *   hide   <id>                                      面板隐藏态（原样保留）
 * 几何量程 ±4096（允许屏外坐标）；id 规则 [A-Za-z0-9_-]、1..15、裸 "-" 除外。
 */
public final class Regions {
    public static final String PATH = "/data/local/vtouch-runtime/regions.conf";

    /** 最近一次解析到的屏帧尺寸（坐标校验用；0 = 未知）。 */
    public static int lastFrameW = 0, lastFrameH = 0;

    public static final class Region {
        public String id = "";
        public int type = 0;
        public int a1, a2, a3, a4;
        public int en = 1;
        public String bindOp = "-";
        public String bindEv = "down";
        public int kind = 0;

        public Region() {}
        public Region(String id) { this.id = id; }

        public Region copy() {
            Region r = new Region(id);
            r.type = type; r.a1 = a1; r.a2 = a2; r.a3 = a3; r.a4 = a4;
            r.en = en; r.bindOp = bindOp; r.bindEv = bindEv; r.kind = kind;
            return r;
        }

        public String typeName() { return type == 0 ? "矩形" : "圆"; }

        public String geomText() {
            if (type == 0) return a1 + "," + a2 + " → " + a3 + "," + a4;
            return "圆心 " + a1 + "," + a2 + "  r=" + a3;
        }

        public String summary() {
            StringBuilder sb = new StringBuilder();
            sb.append(typeName()).append(' ').append(geomText());
            if (en == 0) sb.append("  [停用]");
            if (kind == 1) sb.append("  [开关型]");
            if (!"-".equals(bindOp))
                sb.append("  → ").append(bindOp).append('(')
                  .append("press".equals(bindEv) ? "完整按压" : "按下").append(')');
            return sb.toString();
        }
    }

    public static final class File {
        public String frameLine = null;
        public final List<Region> regions = new ArrayList<Region>();
        public final List<String> hideIds = new ArrayList<String>();
        public final List<String> unknown = new ArrayList<String>();

        public Region find(String id) {
            for (Region r : regions) if (r.id.equals(id)) return r;
            return null;
        }

        public static File parse(String text) {
            File f = new File();
            boolean verSeen = false;
            List<String> pend = new ArrayList<String>();
            for (String rawLine : text.split("\n")) {
                String line = rawLine.trim();
                if (line.isEmpty()) continue;
                if (line.startsWith("#vtouch-regions")) {
                    String v = line.replaceAll("[^0-9]", "");
                    if (!"2".equals(v))
                        throw new IllegalArgumentException("区域文件版本不是 v2：" + line);
                    verSeen = true;
                    continue;
                }
                if (line.startsWith("#frame")) {
                    f.frameLine = line;
                    String[] p = line.trim().split("\\s+");
                    if (p.length >= 4) {
                        try {
                            lastFrameW = Integer.parseInt(p[2]);
                            lastFrameH = Integer.parseInt(p[3]);
                        } catch (Exception e) { /* 忽略 */ }
                    }
                    continue;
                }
                String[] t = line.split("\\s+");
                if (t[0].equals("region") && t.length >= 8) {
                    Region r = new Region(t[1]);
                    r.type = pi(t[2]);
                    r.a1 = pi(t[3]); r.a2 = pi(t[4]); r.a3 = pi(t[5]); r.a4 = pi(t[6]);
                    r.en = pi(t[7]);
                    f.regions.add(r);
                } else {
                    pend.add(line);
                }
            }
            if (!verSeen) throw new IllegalArgumentException("区域文件缺少 #vtouch-regions 版本行");
            for (String line : pend) {
                String[] t = line.split("\\s+");
                if (t[0].equals("bind") && t.length >= 4) {
                    Region r = f.find(t[1]);
                    if (r != null) { r.bindOp = t[2]; r.bindEv = t[3]; }
                    else f.unknown.add(line);
                } else if (t[0].equals("kind") && t.length >= 3) {
                    Region r = f.find(t[1]);
                    if (r != null) r.kind = pi(t[2]);
                    else f.unknown.add(line);
                } else if (t[0].equals("hide") && t.length >= 2) {
                    f.hideIds.add(t[1]);
                } else {
                    f.unknown.add(line);
                }
            }
            return f;
        }

        public String serialize() {
            StringBuilder sb = new StringBuilder();
            sb.append("#vtouch-regions v2\n");
            if (frameLine != null) sb.append(frameLine).append('\n');
            for (Region r : regions) {
                sb.append("region ").append(r.id).append(' ').append(r.type).append(' ')
                  .append(r.a1).append(' ').append(r.a2).append(' ').append(r.a3).append(' ').append(r.a4).append(' ')
                  .append(r.en).append('\n');
                sb.append("bind ").append(r.id).append(' ').append(r.bindOp).append(' ').append(r.bindEv).append('\n');
                sb.append("kind ").append(r.id).append(' ').append(r.kind).append('\n');
            }
            for (String h : hideIds) sb.append("hide ").append(h).append('\n');
            for (String u : unknown) sb.append(u).append('\n');
            return sb.toString();
        }
    }

    /** 校验一条区域（保存前）；self = 被编辑的原对象（新建传 null）。通过返回 null。 */
    public static String validate(Region r, File f, Region self) {
        if (!idOk(r.id)) return "区域 id 非法：字符集 [A-Za-z0-9_-]、长度 1..15、不能是裸 \"-\"";
        for (Region o : f.regions)
            if (o != self && o.id.equals(r.id)) return "区域 id 已存在：" + r.id;
        if (r.type != 0 && r.type != 1) return "类型只能是矩形(0)或圆(1)";
        for (int v : new int[]{r.a1, r.a2, r.a3, r.a4})
            if (v < -4096 || v > 4096) return "坐标超出 ±4096";
        if (r.type == 1 && r.a3 < 0) return "圆的半径不能为负";
        if (!"down".equals(r.bindEv) && !"press".equals(r.bindEv)) return "时机只能是 down / press";
        if (!"-".equals(r.bindOp) && !idOk(r.bindOp)) return "绑定操作名非法";
        if (r.kind != 0 && r.kind != 1) return "开关型只能是 0 / 1";
        if (r.en != 0 && r.en != 1) return "启用只能是 0 / 1";
        return null;
    }

    public static boolean idOk(String s) {
        if (s == null || s.length() < 1 || s.length() > 15) return false;
        if (s.equals("-")) return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        }
        return true;
    }

    static int pi(String s) { return Integer.parseInt(s.trim()); }
}
