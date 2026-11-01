package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * V8 T6 脚本存取与模式锁（编辑器侧，纯逻辑）：
 *  - `#模式 块/脚本` 侧线：单模锁定的持久形态（保存时定锁；缺省按可投影性推导，见 OpsPage 路由）
 *  - prepare：侧线归一 → 静态检查 → 至少 1 条语句 → 编译 → 字节（错误带行号）
 *  - updateMetaLine：ops.conf 行级更新（只替换该操作的元数据行，其余行逐字节保留）
 *  - regionRefs / remapRegions：区域引用扫描与改写（CrossCopy v8 链路：依赖携带 / 改名改写）
 * 纯 Java、零 Android 依赖（设备测试台「T6 双视图」核覆盖）。
 */
public final class ScriptStore {

    private ScriptStore() {}

    // ================= 模式锁（#模式 侧线） =================

    /** 读模式侧线："块" / "脚本" / null（缺省）。只看独立成行的 #模式 行（任意位置取首条）。 */
    public static String modeOf(String src) {
        if (src == null) return null;
        for (String line : src.split("\n")) {
            String t = line.trim().replaceAll("\\s+", "");
            if (t.equals("#模式块")) return "块";
            if (t.equals("#模式脚本")) return "脚本";
        }
        return null;
    }

    /** 归一侧线：删除全部 #模式 行后在文首写一条（保存时定锁）。 */
    public static String withPragma(String src, String mode) {
        StringBuilder b = new StringBuilder();
        b.append("块".equals(mode) ? "#模式 块" : "#模式 脚本").append('\n');
        if (src != null) {
            for (String line : src.split("\n", -1)) {
                String t = line.trim().replaceAll("\\s+", "");
                if (t.equals("#模式块") || t.equals("#模式脚本")) continue;
                b.append(line).append('\n');
            }
        }
        String s = b.toString();
        while (s.endsWith("\n\n")) s = s.substring(0, s.length() - 1);
        return s;
    }

    // ================= 保存管线 =================

    /** prepare 结果：err != null 时 src/bytes 为 null。 */
    public static final class Save {
        public String err;
        public int line;
        public String src;
        public byte[] bytes;
    }

    /** 存盘前处理：侧线归一 → 静态检查 → 至少 1 条语句 → 编译 → 字节。 */
    public static Save prepare(String srcRaw, String mode) {
        return prepareRaw(withPragma(srcRaw, mode));
    }

    /** 同 prepare，但不做侧线处理（文本即最终形态；跨方案复制重编译路径用）。 */
    public static Save prepareRaw(String src) {
        Save s = new Save();
        VtCheck.R ck = VtCheck.run(src);
        if (ck.err != null) { s.err = ck.err; s.line = ck.line; return s; }
        VtParser.R pp = VtParser.parse(src);
        if (pp.err != null) { s.err = pp.err; s.line = pp.line; return s; }
        if (pp.file.sts.isEmpty()) { s.err = "操作没有任何语句（至少 1 条）"; s.line = 0; return s; }
        VtCompiler.R c = VtCompiler.compile(src);
        if (c.err != null) { s.err = c.err; s.line = c.line; return s; }
        s.bytes = VtBc.write(c.blob);
        s.src = src;
        return s;
    }

    // ================= 格式化（v36：编辑器 [格式化]） =================

    /** [格式化] 结果。 */
    public static final class Fmt {
        public String err;    // null = ok
        public int line;      // 错误行（0=无行号）
        public String src;    // 规范形态文本
    }

    /** 注释扫描（字符串感知；`#模式` 侧线也按普通注释收，位置随文——归一留给 withPragma）。行升序。 */
    public static List<VtPrint.Cmt> comments(String src) {
        List<VtPrint.Cmt> out = new ArrayList<VtPrint.Cmt>();
        boolean inStr = false;
        int line = 1;
        for (int i = 0; i < src.length(); i++) {
            char ch = src.charAt(i);
            if (ch == '\n') { line++; inStr = false; continue; }
            if (inStr) {
                if (ch == '\\') { i++; continue; }
                if (ch == '"') inStr = false;
                continue;
            }
            if (ch == '"') { inStr = true; continue; }
            if (ch == '#') {
                int e = src.indexOf('\n', i);
                if (e < 0) e = src.length();
                out.add(new VtPrint.Cmt(line, src.substring(i, e)));
                i = e - 1;
            }
        }
        return out;
    }

    /** 规范重排：解析 → 规范打印（注释随属主语句上方保留；块尾注释留在块内）→ 侧线按原值保留。
     *  只做语法（解析失败报行号）；草稿/未通过静态检查的文本也能排。 */
    public static Fmt format(String srcRaw) {
        Fmt f = new Fmt();
        String src = srcRaw == null ? "" : srcRaw;
        VtParser.R pp = VtParser.parse(src);
        if (pp.err != null) { f.err = pp.err; f.line = pp.line; return f; }
        String mode = modeOf(src);
        String printed = VtPrint.print(pp.file, comments(src));
        f.src = mode != null ? withPragma(printed, mode) : printed;
        return f;
    }

    // ================= ops.conf 行级更新 =================

    /** 更新 ops.conf 文本中某操作的元数据行（行级替换，其余行逐字节保留）；无该行则追加到末尾。 */
    public static String updateMetaLine(String confText, OpsV8.Rec rec) {
        String line = metaLine(rec);
        String[] lines = (confText == null ? "" : confText).split("\n", -1);
        StringBuilder b = new StringBuilder();
        boolean replaced = false;
        for (int i = 0; i < lines.length; i++) {
            String ln = lines[i];
            if (i == lines.length - 1 && ln.isEmpty()) break;   // 结尾换行元素
            if (!replaced) {
                String[] tt = ln.trim().split("\\s+");
                if (tt.length >= 2 && tt[0].equals("op") && tt[1].equals(rec.name)) {
                    b.append(line).append('\n');
                    replaced = true;
                    continue;
                }
            }
            b.append(ln).append('\n');
        }
        if (!replaced) b.append(line).append('\n');
        return b.toString();
    }

    /** 单条元数据行（与 OpsV8.serializeMeta 同格式，extra 原样保留）。 */
    public static String metaLine(OpsV8.Rec r) {
        StringBuilder sb = new StringBuilder();
        sb.append("op ").append(r.name).append(" gate ").append(r.gate)
          .append(" autooff ").append(r.autooff)
          .append(" prog ").append(r.hash).append(' ').append(r.size);
        if (r.extra != null) sb.append(' ').append(r.extra);
        return sb.toString();
    }

    // ================= 区域引用扫描 / 改写（CrossCopy v8 链路） =================

    /** 扫描脚本中的区域引用（在区域/开关开 首参、找图/找色/找色点 区域=、框区域 id）；解析失败返回空表。 */
    public static List<String> regionRefs(String src) {
        List<String> out = new ArrayList<String>();
        VtParser.R pp = VtParser.parse(src == null ? "" : src);
        if (pp.err != null) return out;
        refs(pp.file.sts, out);
        return out;
    }

    /** 区域参考改写：命中 ren 的引用改写后按规范重排打印；解析失败返回 null。 */
    public static String remapRegions(String src, Map<String, String> ren) {
        VtParser.R pp = VtParser.parse(src == null ? "" : src);
        if (pp.err != null) return null;
        remap(pp.file.sts, ren);
        return VtPrint.print(pp.file);
    }

    /** 纯字符串字面量 → 文本（含插值返回 null）。 */
    public static String pureStr(VtAst.StrEx s) {
        StringBuilder b = new StringBuilder();
        for (Object seg : s.segs) {
            if (seg instanceof String) b.append((String) seg);
            else return null;
        }
        return b.toString();
    }

    private static void refs(List<VtAst.St> sts, List<String> out) {
        for (VtAst.St s : sts) {
            if (s instanceof VtAst.AssignSt) {
                refsEx(((VtAst.AssignSt) s).val, out);
            } else if (s instanceof VtAst.IfSt) {
                refsEx(((VtAst.IfSt) s).cond, out);
                refs(((VtAst.IfSt) s).thenB.sts, out);
                if (((VtAst.IfSt) s).elseB != null) refs(((VtAst.IfSt) s).elseB.sts, out);
            } else if (s instanceof VtAst.LoopSt) {
                refs(((VtAst.LoopSt) s).body.sts, out);
            } else if (s instanceof VtAst.WhileSt) {
                refsEx(((VtAst.WhileSt) s).cond, out);
                refs(((VtAst.WhileSt) s).body.sts, out);
            } else if (s instanceof VtAst.ToastSt) {
                refsEx(((VtAst.ToastSt) s).msg, out);
                optRefs(((VtAst.ToastSt) s).opts, out);
            } else if (s instanceof VtAst.HlSt) {
                VtAst.HlSt h = (VtAst.HlSt) s;
                if (h.region != null && !h.region.isEmpty()) out.add(h.region);
                optRefs(h.opts, out);
            } else if (s instanceof VtAst.BoxSt) {
                VtAst.BoxSt x = (VtAst.BoxSt) s;
                refsEx(x.x1, out); refsEx(x.y1, out); refsEx(x.x2, out); refsEx(x.y2, out);
                optRefs(x.opts, out);
            } else if (s instanceof VtAst.FnSt) {
                refs(((VtAst.FnSt) s).body.sts, out);
            } else if (s instanceof VtAst.CallSt) {
                refsEx(((VtAst.CallSt) s).call, out);
            }
        }
    }

    private static void refsEx(VtAst.Ex e, List<String> out) {
        if (e == null) return;
        if (e instanceof VtAst.CallEx) {
            VtAst.CallEx c = (VtAst.CallEx) e;
            if ((c.name.equals("在区域") || c.name.equals("开关开")) && c.args.size() >= 1) addStr(c.args.get(0), out);
            optRefs(c.opts, out);
            for (VtAst.Ex a : c.args) refsEx(a, out);
        } else if (e instanceof VtAst.BinEx) {
            refsEx(((VtAst.BinEx) e).l, out);
            refsEx(((VtAst.BinEx) e).r, out);
        } else if (e instanceof VtAst.UnEx) {
            refsEx(((VtAst.UnEx) e).e, out);
        } else if (e instanceof VtAst.IdxEx) {
            refsEx(((VtAst.IdxEx) e).idx, out);
        }
    }

    private static void optRefs(List<VtAst.Opt> os, List<String> out) {
        if (os == null) return;
        for (VtAst.Opt o : os) if ("区域".equals(o.key)) addStr(o.val, out);
    }

    private static void addStr(VtAst.Ex e, List<String> out) {
        if (e instanceof VtAst.StrEx) {
            String s = pureStr((VtAst.StrEx) e);
            if (s != null && !s.isEmpty()) out.add(s);
        }
    }

    private static void remap(List<VtAst.St> sts, Map<String, String> ren) {
        for (VtAst.St s : sts) {
            if (s instanceof VtAst.AssignSt) {
                remapEx(((VtAst.AssignSt) s).val, ren);
            } else if (s instanceof VtAst.IfSt) {
                remapEx(((VtAst.IfSt) s).cond, ren);
                remap(((VtAst.IfSt) s).thenB.sts, ren);
                if (((VtAst.IfSt) s).elseB != null) remap(((VtAst.IfSt) s).elseB.sts, ren);
            } else if (s instanceof VtAst.LoopSt) {
                remap(((VtAst.LoopSt) s).body.sts, ren);
            } else if (s instanceof VtAst.WhileSt) {
                remapEx(((VtAst.WhileSt) s).cond, ren);
                remap(((VtAst.WhileSt) s).body.sts, ren);
            } else if (s instanceof VtAst.ToastSt) {
                remapEx(((VtAst.ToastSt) s).msg, ren);
                remapOpts(((VtAst.ToastSt) s).opts, ren);
            } else if (s instanceof VtAst.HlSt) {
                VtAst.HlSt h = (VtAst.HlSt) s;
                if (h.region != null && ren.containsKey(h.region)) h.region = ren.get(h.region);
                remapOpts(h.opts, ren);
            } else if (s instanceof VtAst.BoxSt) {
                VtAst.BoxSt x = (VtAst.BoxSt) s;
                remapEx(x.x1, ren); remapEx(x.y1, ren); remapEx(x.x2, ren); remapEx(x.y2, ren);
                remapOpts(x.opts, ren);
            } else if (s instanceof VtAst.FnSt) {
                remap(((VtAst.FnSt) s).body.sts, ren);
            } else if (s instanceof VtAst.CallSt) {
                remapEx(((VtAst.CallSt) s).call, ren);
            }
        }
    }

    private static void remapEx(VtAst.Ex e, Map<String, String> ren) {
        if (e == null) return;
        if (e instanceof VtAst.CallEx) {
            VtAst.CallEx c = (VtAst.CallEx) e;
            if ((c.name.equals("在区域") || c.name.equals("开关开")) && c.args.size() >= 1) setStr(c.args.get(0), ren);
            remapOpts(c.opts, ren);
            for (VtAst.Ex a : c.args) remapEx(a, ren);
        } else if (e instanceof VtAst.BinEx) {
            remapEx(((VtAst.BinEx) e).l, ren);
            remapEx(((VtAst.BinEx) e).r, ren);
        } else if (e instanceof VtAst.UnEx) {
            remapEx(((VtAst.UnEx) e).e, ren);
        } else if (e instanceof VtAst.IdxEx) {
            remapEx(((VtAst.IdxEx) e).idx, ren);
        }
    }

    private static void remapOpts(List<VtAst.Opt> os, Map<String, String> ren) {
        if (os == null) return;
        for (VtAst.Opt o : os) if ("区域".equals(o.key)) setStr(o.val, ren);
    }

    private static void setStr(VtAst.Ex e, Map<String, String> ren) {
        if (!(e instanceof VtAst.StrEx)) return;
        VtAst.StrEx s = (VtAst.StrEx) e;
        String v = pureStr(s);
        if (v == null || !ren.containsKey(v)) return;
        String nv = ren.get(v);
        s.segs.clear();
        s.segs.add(nv);
        s.raw = nv;                 // VtPrint 优先 raw（打印=raw 非 null 时直出）——同步改写
    }
}
