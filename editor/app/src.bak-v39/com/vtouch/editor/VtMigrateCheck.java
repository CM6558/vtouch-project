package com.vtouch.editor;

import java.util.HashSet;
import java.util.Set;

/** VtMigrate 离机核（T5.2）：迁移产物对账——序列化→重解析→**面板同款产物校验**（OpsV8.checkArtifact），
 *  含 拒译保留 / 已 v8 携带 / 混合文件 / 真实 op 全链 / 容器可读回。 */
public final class VtMigrateCheck {

    private static int pass = 0, fail = 0;

    public static void main(String[] args) {
        /* 1) 基本：一步点按 → v8 元数据行 + 产物 */
        VtMigrate.R r = VtMigrate.run(file(op("a1", "-", 0, st(1, 540, 1584, 0, 0, 50, "", 0, 0, ""))));
        ck("基本·迁移计数", r.migrated == 1 && r.kept == 0 && r.carried == 0 && r.outs.size() == 1 && r.refused.isEmpty());
        VtMigrate.OpOut o = r.outs.get(0);
        ck("基本·哈希一致性", o.hash8.equals(OpsV8.fnv8(o.vtbc)) && o.vtbc.length > 0 && o.vtbc.length <= OpsV8.PROG_MAX_BYTES);
        ck("基本·容器可读回", VtBc.read(o.vtbc).err == null);
        String v8 = r.v8.serialize();
        ck("基本·表头 v8", v8.startsWith("#vtouch-ops v8\n") && v8.indexOf("\nstep ") < 0);
        Ops.File back = Ops.File.parse(v8);
        Ops.Op bo = back.find("a1");
        ck("基本·重解析对账", bo != null && o.hash8.equals(bo.progHash) && bo.progSize == o.vtbc.length && bo.steps.isEmpty());
        ck("基本·面板同款产物校验", artifactOk("a1", "-", 0, o) == null);

        /* 2) 拒译：未知行 → 保留步骤行（绝不丢数据） */
        Ops.File f2 = file(op("a1", "-", 0, raw("step 99 1 2 3 4 5 x 0 0 -")));
        VtMigrate.R r2 = VtMigrate.run(f2);
        ck("拒译·保留", r2.migrated == 0 && r2.kept == 1 && r2.outs.isEmpty() && r2.refused.size() == 1);
        String v8b = r2.v8.serialize();
        ck("拒译·步骤行留存", v8b.startsWith("#vtouch-ops v8") && v8b.contains("step 99 1 2 3 4 5 x 0 0 -"));
        ck("拒译·重解析", Ops.File.parse(v8b).find("a1") != null);

        /* 3) 已 v8 操作：原样携带 */
        Ops.Op p = op("p1", "-", 0);
        p.progHash = "deadbeef";
        p.progSize = 100;
        VtMigrate.R r3 = VtMigrate.run(file(p));
        ck("携带·计数", r3.carried == 1 && r3.migrated == 0 && r3.kept == 0);
        Ops.File back3 = Ops.File.parse(r3.v8.serialize());
        ck("携带·行对账", "deadbeef".equals(back3.find("p1").progHash) && back3.find("p1").progSize == 100);

        /* 4) 混合：一条好 + 一条拒 → 同文件双格式共存 */
        Ops.File f4 = file(op("good", "-", 1, st(3, 0, 0, 0, 0, 200, "", 0, 0, "")),
                           op("bad", "r1", 0, raw("step 99 0 0 0 0 0 - 0 0 -")));
        VtMigrate.R r4 = VtMigrate.run(f4);
        ck("混合·计数", r4.migrated == 1 && r4.kept == 1);
        String v8c = r4.v8.serialize();
        Ops.File back4 = Ops.File.parse(v8c);
        ck("混合·双行共存", back4.find("good").progHash != null && back4.find("good").progSize > 0
                && !back4.find("bad").steps.isEmpty() && v8c.contains("\nstep "));
        ck("混合·产物校验", artifactOk("good", "-", 1, r4.outs.get(0)) == null);

        /* 5) 名字非法 → 拒译（不产出半坏行） */
        VtMigrate.R r5 = VtMigrate.run(file(op("", "-", 0, st(3, 0, 0, 0, 0, 100, "", 0, 0, ""))));
        ck("名字非法·拒译", r5.migrated == 0 && r5.kept == 1);

        /* 6) 真实 op1 全链 */
        Ops.Op op1 = op("op1", "r1", 1,
            st(14, 0, -3, -4, 0, 0, "c1", 0, 0, ""),
            st(15, 0, 0, 0, 0, 0, "", 0, 0, ""),
            st(12, 1, 2000, 0, 0, 0, "", 0, 0, ""),
            st(14, 3, 1, 0, 0, 0, "lb1", 0, 0, ""),
            st(9, 0, 0, 0, 0, 0, "e1", 0, 0, "tdx + cos(atan2(tuy-tdy, tux-tdx)) *200"),
            st(9, 1, 0, 0, 0, 0, "e2", 0, 0, "tdy + sin(atan2(tuy-tdy, tux-tdx)) *200"),
            st(2, -1, -2, -10, -11, 100, "", 30, 0, ""),
            st(1, 154, 1978, 0, 0, 50, "", 0, 0, ""),
            st(17, 0, 0, 0, 0, 0, "", 0, 0, ""),
            st(16, 0, 0, 0, 0, 0, "", 0, 0, ""),
            st(3, 0, 0, 0, 0, 50, "", 0, 0, ""),
            st(13, 0, 0, 0, 0, 0, "", 0, 0, ""),
            st(16, 0, 0, 0, 0, 0, "", 0, 0, ""));
        VtMigrate.R r6 = VtMigrate.run(file(op1));
        ck("真实 op1·迁移", r6.migrated == 1 && r6.refused.isEmpty());
        ck("真实 op1·产物校验", artifactOk("op1", "r1", 1, r6.outs.get(0)) == null);
        ck("真实 op1·源哈希对账", VtBc.hash(r6.outs.get(0).vts) == VtBc.read(r6.outs.get(0).vtbc).blob.srcHash);

        System.out.println("VtMigrateCheck: 通过 " + pass + " · 失败 " + fail);
        if (fail > 0) System.exit(1);
    }

    /* ================= 工具 ================= */

    private static String artifactOk(String name, String gate, int autooff, VtMigrate.OpOut o) {
        OpsV8.Rec rec = new OpsV8.Rec();
        rec.name = name;
        rec.gate = gate;
        rec.autooff = autooff;
        rec.hash = o.hash8;
        rec.size = o.vtbc.length;
        String v = OpsV8.validRec(rec);
        if (v != null) return "元数据非法：" + v;
        return OpsV8.checkArtifact(rec, o.vtbc);
    }

    private static Ops.Step st(int type, int a1, int a2, int a3, int a4, int ms, String ref, int j1, int j2, String expr) {
        Ops.Step s = new Ops.Step();
        s.type = type; s.a1 = a1; s.a2 = a2; s.a3 = a3; s.a4 = a4; s.ms = ms;
        s.ref = ref; s.j1 = j1; s.j2 = j2; s.expr = expr;
        return s;
    }

    private static Ops.Step raw(String line) {
        Ops.Step s = new Ops.Step();
        s.raw = line;
        return s;
    }

    private static Ops.Op op(String name, String gate, int autooff, Ops.Step... ss) {
        Ops.Op o = new Ops.Op();
        o.name = name;
        o.gate = gate;
        o.autooff = autooff;
        for (Ops.Step s : ss) o.steps.add(s);
        return o;
    }

    private static Ops.File file(Ops.Op... os) {
        Ops.File f = new Ops.File();
        f.version = 5;
        f.header = "#vtouch-ops v5";
        for (Ops.Op o : os) f.ops.add(o);
        return f;
    }

    private static void ck(String name, boolean ok) {
        if (ok) { pass++; System.out.println("[OK] " + name); }
        else { fail++; System.out.println("[FAIL] " + name); }
    }
}
