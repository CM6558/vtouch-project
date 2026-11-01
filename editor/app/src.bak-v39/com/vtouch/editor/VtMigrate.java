package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * v5 → v8 迁移核心（T5.2；纯逻辑、无文件 I/O，编辑器与离机核共用）。
 *
 * 输入 = Ops.File（v5 / 混合）；每条步骤操作：`VtFromSteps` 翻译 → `VtCompiler` 编译 →
 * 产出 `.vts` 源 + `.vtbc` 字节 + 元数据（FNV-1a 32 哈希 / 大小）。
 * 输出 = v8 File 模型：迁移成功 → 元数据行（`op 名 gate g autooff a prog 哈希 大小`）；
 * 不可译 / 名字非法 → **原样保留步骤行**（VTSTORE_V8 §2 过渡期双格式合法，绝不丢数据）并记报告。
 * 规范：docs/VTSTORE_V8.md §2/§3。
 */
public final class VtMigrate {

    /** 一条迁移成功操作的产物（runner 写 progs/ 用）。 */
    public static final class OpOut {
        public String name;
        public String vts;      // 源码（规范形态）
        public byte[] vtbc;     // 编译产物（≤32768）
        public String hash8;    // FNV-1a 32(.vtbc 字节) → 8 位小写 hex
    }

    public static final class R {
        public Ops.File v8;                                          // 结果文件模型（header=v8）
        public final List<OpOut> outs = new ArrayList<OpOut>();      // 迁移成功 → 需写 progs/
        public final List<String> refused = new ArrayList<String>(); // 「名：原因」→ 保留步骤行
        public final List<String> notes = new ArrayList<String>();   // 翻译器 notes（自动命名等）
        public int migrated, carried, kept;
    }

    public static R run(Ops.File f) {
        R r = new R();
        Ops.File nf = new Ops.File();
        nf.version = 8;
        nf.header = "#vtouch-ops v8";
        nf.unknown.addAll(f.unknown);                    // 文件级未知行原样保序归档
        r.v8 = nf;
        for (Ops.Op o : f.ops) {
            if (o.progHash != null) {                    // 已是 v8 程序操作 → 原样携带
                nf.ops.add(o);
                r.carried++;
                continue;
            }
            if (o.name == null || o.name.isEmpty() || !Regions.idOk(o.name)) {
                nf.ops.add(o);
                r.refused.add((o.name == null || o.name.isEmpty() ? "(无名)" : o.name) + "：操作名非法（v11 尺子）");
                r.kept++;
                continue;
            }
            VtFromSteps.R t = VtFromSteps.translate(o);
            if (t.src == null) {
                nf.ops.add(o);                           // 保留步骤行（过渡期合法）
                r.refused.add(o.name + "：" + (t.problems.isEmpty() ? "未知原因" : t.problems.get(0)));
                r.kept++;
                continue;
            }
            VtCompiler.R cp = VtCompiler.compile(t.src);
            if (cp.err != null || cp.blob == null) {     // 防御：翻译器自检已编译过一轮，理论不可达
                nf.ops.add(o);
                r.refused.add(o.name + "：编译失败 " + (cp.err == null ? "?" : cp.err));
                r.kept++;
                continue;
            }
            byte[] bytes = VtBc.write(cp.blob);
            OpOut out = new OpOut();
            out.name = o.name;
            out.vts = t.src;
            out.vtbc = bytes;
            out.hash8 = OpsV8.fnv8(bytes);
            r.outs.add(out);
            for (String n : t.notes) r.notes.add(o.name + "：" + n);
            Ops.Op no = new Ops.Op();
            no.name = o.name;
            no.gate = o.gate;
            no.autooff = o.autooff;
            no.progHash = out.hash8;
            no.progSize = bytes.length;
            no.extra = o.extra;                          // 尾段原样保（正常 null）
            nf.ops.add(no);
            r.migrated++;
        }
        return r;
    }
}
