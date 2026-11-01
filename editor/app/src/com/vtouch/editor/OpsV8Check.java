package com.vtouch.editor;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** OpsV8 离机核（T4.1）：FNV 向量 / 元数据行解析·序列化往返 / 语义校验 / 对账比较器。
 *  运行：~/vtchk-host/run.sh（javac + HostCheck + VtScriptCheck + OpsV8Check）。 */
public final class OpsV8Check {
    static int fails = 0;

    static void chk(String name, boolean ok) {
        System.out.println((ok ? "[OK]   " : "[FAIL] ") + name);
        if (!ok) fails++;
    }

    static byte[] u8(String s) {
        try { return s.getBytes("UTF-8"); } catch (Exception e) { return s.getBytes(); }
    }

    public static void main(String[] args) {
        try { run(); } catch (Throwable t) { t.printStackTrace(); fails++; }
        System.out.println(fails == 0 ? "== ALL PASS ==" : "== FAILS: " + fails + " ==");
        System.exit(fails == 0 ? 0 : 1);
    }

    static void run() {
        // ---------- FNV-1a 32（与 VtBc.hash 同式；标准测试向量） ----------
        chk("FNV-1a 32：空 = 0x811C9DC5", OpsV8.fnv32(new byte[0]) == 0x811C9DC5);
        chk("FNV-1a 32：\"a\" = 0xE40C292C", OpsV8.fnv32(u8("a")) == 0xE40C292C);
        chk("FNV-1a 32：\"foobar\" = 0xBF9CF968", OpsV8.fnv32(u8("foobar")) == 0xBF9CF968);
        chk("FNV 与 VtBc.hash 同式（交叉）", OpsV8.fnv32(u8("点按 100 200 50")) == VtBc.hash("点按 100 200 50"));
        chk("fnv8 形态：8 位小写 hex", "e40c292c".equals(OpsV8.fnv8(u8("a")))
                && "811c9dc5".equals(OpsV8.fnv8(new byte[0])));
        chk("hex8Ok：合法/大写拒/长度拒", OpsV8.hex8Ok("deadbeef") && !OpsV8.hex8Ok("DEADBEEF")
                && !OpsV8.hex8Ok("deadbee") && !OpsV8.hex8Ok("deadbeeg"));

        // ---------- 解析 / 序列化往返（规范形态逐字） ----------
        String meta = "#vtouch-ops v8\n"
                + "op 补刀 gate s1 autooff 1 prog 0123abcd 166\n"
                + "op ztp1 gate - autooff 0 prog deadbeef 166\n";
        OpsV8.MetaFile f1 = OpsV8.parseMeta(meta);
        chk("元数据解析：2 条 + 逐字往返", f1 != null && f1.recs.size() == 2
                && meta.equals(OpsV8.serializeMeta(f1)));
        chk("字段坐实：name/gate/autooff/hash/size",
                f1.recs.get(0).name.equals("补刀") && f1.recs.get(0).gate.equals("s1")
                        && f1.recs.get(0).autooff == 1 && f1.recs.get(0).hash.equals("0123abcd")
                        && f1.recs.get(0).size == 166
                        && f1.recs.get(1).gate.equals("-") && f1.recs.get(1).autooff == 0);
        chk("find 按名定位", f1.find("ztp1") != null && f1.find("不存在") == null);

        // 混合文件（过渡期）：步骤行 / 空行 / 未知行进 others 原样；元数据照取
        String mixed = "#vtouch-ops v8\n"
                + "op zt1 gate - autooff 0\n"
                + "step 1 300 400 0 0 300 - 0 0 -\n"
                + "op ztp1 gate - autooff 0 prog deadbeef 166\n"
                + "未来行 x y\n";
        OpsV8.MetaFile f2 = OpsV8.parseMeta(mixed);
        chk("混合文件：元数据 1 条 + others 3 行（步骤/未来行）",
                f2 != null && f2.recs.size() == 1 && f2.recs.get(0).name.equals("ztp1")
                        && f2.others.size() == 3 && f2.others.get(0).startsWith("op zt1")
                        && f2.others.get(1).startsWith("step "));
        chk("混合序列化：recs 前置 + others 原样保序",
                OpsV8.serializeMeta(f2).equals("#vtouch-ops v8\n"
                        + "op ztp1 gate - autooff 0 prog deadbeef 166\n"
                        + "op zt1 gate - autooff 0\n"
                        + "step 1 300 400 0 0 300 - 0 0 -\n"
                        + "未来行 x y\n"));

        // v5 文件（无元数据）：recs 空、header 保留；坏数字段 → others 原样
        OpsV8.MetaFile f3 = OpsV8.parseMeta("#vtouch-ops v5\nop q1 gate - autooff 0\n");
        chk("v5 文件：无元数据记录（others 收留）", f3 != null && f3.recs.isEmpty()
                && "#vtouch-ops v5".equals(f3.header));
        OpsV8.MetaFile f4 = OpsV8.parseMeta("#vtouch-ops v8\nop x gate - autooff ?? prog deadbeef 1\n");
        chk("数字段坏 → others（不炸）", f4 != null && f4.recs.isEmpty() && f4.others.size() == 1);
        OpsV8.MetaFile f5 = OpsV8.parseMeta("垃圾文件\n没有版本行\n");
        chk("非 ops 文件 → recs 空 + header null", f5 != null && f5.header == null && f5.recs.isEmpty());
        chk("null 输入 → null", OpsV8.parseMeta(null) == null);

        // extra 尾段保留（未来扩展）
        OpsV8.MetaFile f6 = OpsV8.parseMeta("#vtouch-ops v8\nop z1 gate - autooff 0 prog deadbeef 166 未来 7\n");
        chk("extra 尾段保留 + 往返", f6.recs.size() == 1 && "未来 7".equals(f6.recs.get(0).extra)
                && OpsV8.serializeMeta(f6).contains("prog deadbeef 166 未来 7"));

        // ---------- 语义校验（写前门） ----------
        chk("validRec：正例（中文名/门控/无门控）", recOk("补刀", "s1", 1, "0123abcd", 166)
                && recOk("ztp1", "-", 0, "deadbeef", 1) && recOk("ztp1", "-", 0, "deadbeef", 32768));
        chk("validRec：名字拒（空/裸-/空白/超15字）", !recOk("", "-", 0, "deadbeef", 1)
                && !recOk("-", "-", 0, "deadbeef", 1) && !recOk("a b", "-", 0, "deadbeef", 1)
                && !recOk("一二三四五六七八九十一二三四五六", "-", 0, "deadbeef", 1));
        chk("validRec：门控拒（裸 - 以外的坏名）", !recOk("z1", "a/b", 0, "deadbeef", 1));
        chk("validRec：autooff 域（2 拒）", !recOk("z1", "-", 2, "deadbeef", 1));
        chk("validRec：哈希拒（大写/短/非 hex）", !recOk("z1", "-", 0, "DEADBEEF", 1)
                && !recOk("z1", "-", 0, "deadbee", 1) && !recOk("z1", "-", 0, "deadbeeg", 1));
        chk("validRec：大小域（0/32769 拒）", !recOk("z1", "-", 0, "deadbeef", 0)
                && !recOk("z1", "-", 0, "deadbeef", 32769));

        // ---------- 产物完整性（面板同款口径） ----------
        OpsV8.Rec r = mkRec("z1", "-", 0, OpsV8.fnv8(u8("VTBC")), 4);
        chk("checkArtifact：正例", OpsV8.checkArtifact(r, u8("VTBC")) == null);
        chk("checkArtifact：缺产物", "缺产物".equals(OpsV8.checkArtifact(r, null))
                && "缺产物".equals(OpsV8.checkArtifact(r, new byte[0])));
        chk("checkArtifact：大小不符", OpsV8.checkArtifact(r, u8("VTB")) != null
                && OpsV8.checkArtifact(r, u8("VTBCX")).toString().startsWith("大小不符"));
        chk("checkArtifact：哈希不符", OpsV8.checkArtifact(mkRec("z1", "-", 0, "00000000", 4), u8("VTBC")).toString().startsWith("哈希不符"));

        // ---------- 对账比较器 ----------
        Map<String, String> a = new HashMap<String, String>(), b = new HashMap<String, String>();
        a.put("ops.conf", "m1"); a.put("regions.conf", "m2"); a.put("progs/x.vtbc", "m3");
        b.put("ops.conf", "m1"); b.put("regions.conf", "m2"); b.put("progs/x.vtbc", "m3");
        chk("diffMaps：全同 → 空", OpsV8.diffMaps(a, b).isEmpty());
        b.put("progs/x.vtbc", "m9");
        chk("diffMaps：内容异 → 异 progs/x.vtbc", OpsV8.diffMaps(a, b).size() == 1
                && OpsV8.diffMaps(a, b).get(0).equals("异 progs/x.vtbc"));
        b.remove("progs/x.vtbc"); b.put("progs/y.vtbc", "m3");
        List<String> d = OpsV8.diffMaps(a, b);
        chk("diffMaps：缺/多（缺先多后，排序稳定）", d.size() == 2 && d.get(0).equals("缺 progs/x.vtbc")
                && d.get(1).equals("多 progs/y.vtbc"));
        List<String> d2 = OpsV8.diffMaps(new HashMap<String, String>(), a);
        chk("diffMaps：一侧全缺", d2.size() == 3 && d2.get(0).startsWith("多 "));

        // ---------- 拷贝独立（防浅拷贝串位） ----------
        OpsV8.Rec r2 = f1.recs.get(0).copy();
        r2.hash = "ffffffff";
        chk("Rec.copy 深拷贝独立", !f1.recs.get(0).hash.equals("ffffffff"));

        // ---------- Ops.java v8 保真（T4.1b：混合文件经步骤模型逐字往返） ----------
        String v8mix = "#vtouch-ops v8\n"
                + "op zt1 gate - autooff 0\n"
                + "step 1 300 400 0 0 300 - 0 0 -\n"
                + "op ztp1 gate s1 autooff 1 prog 0123abcd 166\n";
        Ops.File vo = Ops.File.parse(v8mix);
        boolean vok = vo.version == 8 && vo.find("ztp1") != null && vo.find("zt1") != null
                && "0123abcd".equals(vo.find("ztp1").progHash) && vo.find("ztp1").progSize == 166
                && vo.find("zt1").progHash == null && vo.find("zt1").steps.size() == 1;
        chk("Ops 模型：v8 混合文件解析（步骤 + 程序元数据）", vok);
        chk("Ops 模型：序列化逐字保真", v8mix.equals(vo.serialize()));
        chk("Ops 模型：程序操作 validate 过（0 步合法）", Ops.validate(vo.find("ztp1"), vo, vo.find("ztp1")) == null);
        Ops.Op badp = new Ops.Op();
        badp.name = "zp2"; badp.progHash = "XYZ"; badp.progSize = 10;
        chk("Ops 模型：程序操作坏哈希拒", Ops.validate(badp, vo, badp) != null);
        Ops.Op bigp = new Ops.Op();
        bigp.name = "zp3"; bigp.progHash = "deadbeef"; bigp.progSize = 32769;
        chk("Ops 模型：程序操作大小越界拒", Ops.validate(bigp, vo, bigp) != null);

        // ---------- IO 路径（形态检查，不落盘） ----------
        OpsV8.baseOverride = "/x/root";
        chk("路径：live/scheme/progs 拼装", OpsV8.liveOps().equals("/x/root/ops.conf")
                && OpsV8.schemeProgs("吕布").equals("/x/root/schemes/吕布/progs")
                && OpsV8.progPath("/d", "补刀").equals("/d/progs/补刀.vtbc"));
        OpsV8.baseOverride = null;
        chk("路径：默认回生产", OpsV8.liveProgs().equals("/data/local/vtouch-runtime/progs"));

        if (fails > 0) System.out.println("（失败 " + fails + " 例）");
    }

    static boolean recOk(String name, String gate, int autooff, String hash, int size) {
        return OpsV8.validRec(mkRec(name, gate, autooff, hash, size)) == null;
    }

    static OpsV8.Rec mkRec(String name, String gate, int autooff, String hash, int size) {
        OpsV8.Rec r = new OpsV8.Rec();
        r.name = name; r.gate = gate; r.autooff = autooff; r.hash = hash; r.size = size;
        return r;
    }
}
