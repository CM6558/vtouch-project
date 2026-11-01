package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * ops.conf v8 存储层（T4.1；格式唯一规范 = 仓库 docs/VTSTORE_V8.md）。
 *
 * 盘上布局：live（/data/local/vtouch-runtime）与每个方案目录 schemes/<名>/ 同构：
 *   ops.conf（v8 元数据行 [+ 过渡期步骤行]）+ progs/<操作名>.vts（源码）+ progs/<操作名>.vtbc（产物）。
 * 元数据行：op &lt;名&gt; gate &lt;门控|-> autooff &lt;0|1&gt; prog &lt;哈希8hex&gt; &lt;大小&gt;
 *   哈希 = .vtbc 字节的 FNV-1a 32（与 VtBc.hash / 核心面板同式）；大小 1..32768。
 *
 * 分工：纯逻辑（解析/序列化/校验/FNV/对账比较器）离机可测（~/vtchk-host）；
 * IO 全走 su（与 Schemes/Su 同款）；「镜像对账」= live 与 schemes/<current> 的
 * 两文件 + progs/ 逐文件 md5 比对（空差异 = 一致）。
 */
public final class OpsV8 {
    /** live 基目录（生产）。 */
    public static final String BASE = "/data/local/vtouch-runtime";
    /** 测试重定向：非 null 时整链（live/schemes/progs）换根（照 RegionDraw.pathOverride 口径）。 */
    public static String baseOverride = null;

    public static final String PROG_MAX = "32768";   // = 核心 VT_OP_PROG_MAX（字符串防误用）
    public static final int PROG_MAX_BYTES = 32768;

    public static String base() { return baseOverride != null ? baseOverride : BASE; }
    public static String liveOps() { return base() + "/ops.conf"; }
    public static String liveRegions() { return base() + "/regions.conf"; }
    public static String liveProgs() { return base() + "/progs"; }
    public static String schemeDir(String name) { return base() + "/schemes/" + name; }
    public static String schemeProgs(String name) { return schemeDir(name) + "/progs"; }
    public static String progPath(String dirBase, String opName) { return dirBase + "/progs/" + opName + ".vtbc"; }
    /** 脚本源路径（T6）：progs/<操作名>.vts。 */
    public static String srcPath(String dirBase, String opName) { return dirBase + "/progs/" + opName + ".vts"; }

    // ---------------- 纯逻辑 ----------------

    /** FNV-1a 32（VtBc.hash 同式；byte[] 版）。 */
    public static int fnv32(byte[] b) {
        int h = 0x811C9DC5;
        for (int i = 0; i < b.length; i++) { h ^= (b[i] & 0xFF); h *= 0x01000193; }
        return h;
    }

    /** FNV-1a 32 → 8 位小写 hex（元数据行「哈希」段的标准形态）。 */
    public static String fnv8(byte[] b) { return String.format("%08x", fnv32(b)); }

    public static boolean hex8Ok(String s) {
        if (s == null || s.length() != 8) return false;
        for (int i = 0; i < 8; i++) {
            char c = s.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    /** 一条元数据记录（程序操作）。extra = 第 10 字段起尾段（未来扩展原样保留，正常 null）。 */
    public static final class Rec {
        public String name = "";
        public String gate = "-";
        public int autooff = 0;
        public String hash = "";
        public int size = 0;
        public String extra = null;

        public Rec copy() {
            Rec r = new Rec();
            r.name = name; r.gate = gate; r.autooff = autooff; r.hash = hash; r.size = size; r.extra = extra;
            return r;
        }
    }

    /** 元数据文件模型：header（#vtouch-ops v8）+ recs + others（非元数据行原样保序）。 */
    public static final class MetaFile {
        public String header = null;
        public final List<Rec> recs = new ArrayList<Rec>();
        public final List<String> others = new ArrayList<String>();

        public Rec find(String name) {
            for (Rec r : recs) if (r.name.equals(name)) return r;
            return null;
        }
    }

    /** 解析元数据行（步骤行/空行/未识别行进 others 原样保留；不做语义校验——那是 validRec 的活）。
     *  返回 null = 连版本行都没有（不是 ops 文件）。 */
    public static MetaFile parseMeta(String text) {
        if (text == null) return null;
        MetaFile f = new MetaFile();
        for (String rawLine : text.split("\n")) {
            String line = rawLine.trim();
            if (line.isEmpty()) continue;
            if (line.startsWith("#vtouch-ops")) { f.header = line; continue; }
            String[] t = line.split("\\s+");
            if (t.length >= 9 && t[0].equals("op") && t[2].equals("gate") && t[4].equals("autooff") && t[6].equals("prog")) {
                Rec r = new Rec();
                r.name = t[1];
                r.gate = t[3];
                r.hash = t[7];
                try {
                    r.autooff = Integer.parseInt(t[5]);      /* 原样存（域校验交给 validRec，不在这里悄悄夹取） */
                    r.size = Integer.parseInt(t[8]);
                } catch (Exception e) {
                    f.others.add(line);     // 数字段坏：原样保留（语义校验交给 validRec / 验收）
                    continue;
                }
                if (t.length > 9) {
                    StringBuilder sb = new StringBuilder();
                    for (int i = 9; i < t.length; i++) { if (i > 9) sb.append(' '); sb.append(t[i]); }
                    r.extra = sb.toString();
                }
                f.recs.add(r);
            } else {
                f.others.add(line);
            }
        }
        if (f.header == null && f.recs.isEmpty() && f.others.isEmpty()) return null;
        return f;
    }

    /** 规范序列化：header（缺省 #vtouch-ops v8）+ 元数据行（recs 序）+ others 原样。 */
    public static String serializeMeta(MetaFile f) {
        StringBuilder sb = new StringBuilder();
        sb.append(f.header != null && f.header.startsWith("#vtouch-ops") ? f.header : "#vtouch-ops v8").append('\n');
        for (Rec r : f.recs) {
            sb.append("op ").append(r.name).append(" gate ").append(r.gate)
              .append(" autooff ").append(r.autooff)
              .append(" prog ").append(r.hash).append(' ').append(r.size);
            if (r.extra != null) sb.append(' ').append(r.extra);
            sb.append('\n');
        }
        for (String u : f.others) sb.append(u).append('\n');
        return sb.toString();
    }

    /** 语义校验（写前门）：返回 null 或原因文案（与核心 vt_prog_put 的域对齐）。 */
    public static String validRec(Rec r) {
        if (r == null || r.name.isEmpty()) return "名字为空";
        if (!Regions.idOk(r.name)) return "名字非法（1..15 个可见字符，不能是裸 \"-\"）";
        if (!"-".equals(r.gate) && !Regions.idOk(r.gate)) return "门控区域名非法";
        if (r.autooff != 0 && r.autooff != 1) return "autooff 只能是 0 / 1";
        if (!hex8Ok(r.hash)) return "哈希非法（8 位小写 hex）";
        if (r.size < 1 || r.size > PROG_MAX_BYTES) return "大小越界（1..32768）";
        return null;
    }

    /** 产物完整性校验（面板同款口径）：返回 null / 「缺产物」——调用方先取字节，再叫本函数。 */
    public static String checkArtifact(Rec r, byte[] vtbc) {
        if (vtbc == null || vtbc.length == 0) return "缺产物";
        if (vtbc.length != r.size) return "大小不符（元数据 " + r.size + " / 实际 " + vtbc.length + "）";
        if (!fnv8(vtbc).equals(r.hash)) return "哈希不符（元数据 " + r.hash + " / 实际 " + fnv8(vtbc) + "）";
        return null;
    }

    /** 对账比较器（纯逻辑）：a/b = 相对路径 → md5；返回差异清单（排序；空 = 一致）。 */
    public static List<String> diffMaps(Map<String, String> a, Map<String, String> b) {
        List<String> out = new ArrayList<String>();
        TreeMap<String, String> ta = new TreeMap<String, String>(a);
        TreeMap<String, String> tb = new TreeMap<String, String>(b);
        for (Map.Entry<String, String> e : ta.entrySet()) {
            String v = tb.get(e.getKey());
            if (v == null) out.add("缺 " + e.getKey());
            else if (!v.equals(e.getValue())) out.add("异 " + e.getKey());
        }
        for (Map.Entry<String, String> e : tb.entrySet())
            if (!ta.containsKey(e.getKey())) out.add("多 " + e.getKey());
        return out;
    }

    // ---------------- IO（su；设备侧） ----------------

    /** 读一个目录基（live 或 schemes/<名>）的「两文件 + progs/*」md5 表（相对路径 → md5）。
     *  一次 su（md5sum 批量）；读不到的条目自然缺席（对账按缺席报「缺/多」）。 */
    public static Map<String, String> gatherMd5(String dirBase) {
        Map<String, String> out = new TreeMap<String, String>();
        Su.R r = Su.run("cd '" + dirBase + "' 2>/dev/null && md5sum ops.conf regions.conf progs/* 2>/dev/null");
        for (String line : r.out.split("\n")) {
            line = line.trim();
            int sp = line.indexOf("  ");
            if (sp <= 0) continue;
            String md5 = line.substring(0, sp).trim(), path = line.substring(sp + 2).trim();
            if (md5.length() != 32 || path.isEmpty()) continue;
            if (path.endsWith(": No such file or directory")) continue;
            out.put(path, md5);
        }
        return out;
    }

    /** 镜像对账：live vs schemes/<名>。返回差异清单（空 = 一致）。 */
    public static List<String> audit(String schemeName) {
        return diffMaps(gatherMd5(base()), gatherMd5(schemeDir(schemeName)));
    }

    /** 读 ops.conf 元数据（不存在/解析不了 → null）。 */
    public static MetaFile readMeta(String opsPath) {
        return parseMeta(Su.read(opsPath));
    }

    /** 写 ops.conf 元数据（原子写）。 */
    public static Su.R writeMeta(String opsPath, MetaFile f) {
        return Su.writeAtomic(opsPath, serializeMeta(f).getBytes(java.nio.charset.Charset.forName("UTF-8")));
    }

    /** 读 progs/<名>.vtbc 字节（缺 → null）。 */
    public static byte[] readProg(String dirBase, String opName) {
        return Su.readBytes(progPath(dirBase, opName));
    }

    /** 写 progs/<名>.vtbc（先建目录；原子写）。 */
    public static Su.R writeProg(String dirBase, String opName, byte[] vtbc) {
        Su.R m = Su.run("mkdir -p '" + dirBase + "/progs'");
        if (!m.ok()) return m;
        return Su.writeAtomic(progPath(dirBase, opName), vtbc);
    }

    /** 读 progs/<名>.vts 源码（缺 → null；T6）。 */
    public static String readSrc(String dirBase, String opName) {
        return Su.read(srcPath(dirBase, opName));
    }

    /** 写 progs/<名>.vts 源码（先建目录；原子写；T6）。 */
    public static Su.R writeSrc(String dirBase, String opName, String src) {
        Su.R m = Su.run("mkdir -p '" + dirBase + "/progs'");
        if (!m.ok()) return m;
        return Su.writeAtomic(srcPath(dirBase, opName), src.getBytes(java.nio.charset.Charset.forName("UTF-8")));
    }

    /** 源码、字节码、索引以及当前方案镜像一起发布；不把部分写入误报成成功。 */
    public static Su.R publish(String opName, String src, byte[] bytes, String conf, String opsPath, boolean mirror) {
        if (!Regions.idOk(opName)) return new Su.R(-1, "操作名非法");
        VtBc.R parsed = VtBc.read(bytes);
        if (parsed.err != null || parsed.blob == null || parsed.blob.srcHash != VtBc.hash(src))
            return new Su.R(-1, "源码与编译产物不一致，未发布");
        MetaFile meta = parseMeta(conf);
        Rec rec = meta == null ? null : meta.find(opName);
        if (rec == null || checkArtifact(rec, bytes) != null) return new Su.R(-1, "索引与编译产物不一致，未发布");
        java.util.LinkedHashMap<String, byte[]> files = new java.util.LinkedHashMap<String, byte[]>();
        byte[] source = src.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] config = conf.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        files.put(srcPath(base(), opName), source);
        files.put(progPath(base(), opName), bytes);
        files.put(opsPath, config);
        String cur = mirror ? Schemes.current() : "";
        if (mirror && !Schemes.nameOk(cur)) return new Su.R(-1, "读不到当前方案，未发布");
        String mkdir = "mkdir -p " + PublishTransaction.quote(liveProgs());
        if (mirror) {
            String target = schemeDir(cur);
            mkdir += " && test -d " + PublishTransaction.quote(target)
                    + " && mkdir -p " + PublishTransaction.quote(target + "/progs");
            files.put(srcPath(target, opName), source);
            files.put(progPath(target, opName), bytes);
            files.put(target + "/ops.conf", config);
            files.put(base() + "/.publish-pending", fnv8(config).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
        Su.R dirs = Su.run(mkdir);
        return dirs.ok() ? Su.publish(base(), files, mirror ? cur : null) : dirs;
    }

    /** progs 目录精确同步（src → dst）：拷全量 + 删多余；源目录缺失 = 视空（对齐「缺失份写空」口径）。
     *  目录级 rm+mv 换根（同目录内 rename 原子）；一次 su。 */
    public static Su.R mirrorProgs(String srcBase, String dstBase) {
        String src = srcBase + "/progs", dst = dstBase + "/progs";
        String stage = dst + ".stage-" + Long.toHexString(System.nanoTime());
        String old = stage + "-old";
        String cmd = "set -e\nmkdir " + PublishTransaction.quote(stage)
                + "\nif [ -d " + PublishTransaction.quote(src) + " ]; then cp -R " + PublishTransaction.quote(src + "/.")
                + " " + PublishTransaction.quote(stage) + "; fi\nfind " + PublishTransaction.quote(stage)
                + " -type f -exec chmod 644 {} +\n"
                + "if [ -e " + PublishTransaction.quote(dst) + " ]; then mv " + PublishTransaction.quote(dst)
                + " " + PublishTransaction.quote(old) + "; fi\n"
                + "if mv " + PublishTransaction.quote(stage) + " " + PublishTransaction.quote(dst) + "; then\n"
                + "rm -rf " + PublishTransaction.quote(old) + "\nelse\n"
                + "if [ -d " + PublishTransaction.quote(old) + " ]; then mv " + PublishTransaction.quote(old)
                + " " + PublishTransaction.quote(dst) + "; fi\nexit 1\nfi";
        return Su.run(cmd);
    }

    /** .bak-v8 备份（迁移/批量改动前）：ops.conf / regions.conf 单备份 + progs 目录整体。
     *  返回各件的备份后校验行（md5sum，供归档报告）。
     *  注意：各条件段用 `;` 独立语句（不用 `&&` 串）——缺 regions.conf 时不许中断后面的 progs 备份
     *  （toybox sh 无 set -e，独立语句各自容错；首段 cd 失败才 exit）。 */
    public static Su.R backupV8(String dirBase) {
        String cmd = "cd '" + dirBase + "' || exit 1;"
                + " [ -f ops.conf ] && cp -f ops.conf ops.conf.bak-v8;"
                + " [ -f regions.conf ] && cp -f regions.conf regions.conf.bak-v8;"
                + " rm -rf progs.bak-v8;"
                + " [ -d progs ] && cp -r progs progs.bak-v8;"
                + " md5sum ops.conf.bak-v8 regions.conf.bak-v8 2>/dev/null; md5sum progs.bak-v8/* 2>/dev/null; true";
        return Su.run(cmd);
    }
}
