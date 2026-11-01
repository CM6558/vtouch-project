package com.vtouch.editor;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * 无头测试台：不依赖任何界面/前台窗口，在应用进程内跑完整批检查（零打扰）。
 *
 * 批次（mode=all，共 10 步）：
 *   1 su 可用性（含核心/方案概况）        2 实时 regions/ops 往返一致性
 *   3 区域合成用例（含负例）              4 操作合成用例（含负例）
 *   5 校验与命名规则                       6 写盘链路（scratch 目录，不碰真数据）
 *   7..10 四页渲染 + 布局断言 + 关键文本存在性（防渲染错页）
 *
 * 产物（files 目录）：test_run.txt（逐行+时间戳）/ test_report.txt / render_p0..3.png / diag_p0..3.txt
 * mode: all（默认）| quick（仅数据）| render（仅渲染）。cb 回调用于外层（胶囊/服务）展示进度。
 */
public final class TestBench {
    private static final String TAG = "VTEditor";

    /** 进度回调：label=步骤名，idx/total=第几步/共几步。 */
    public interface Progress { void step(String label, int idx, int total); }

    private static final Object LOCK = new Object();
    private static boolean running = false;

    public static boolean isRunning() { synchronized (LOCK) { return running; } }

    public static String ts() {
        return new SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(new Date());
    }

    /** 追加一行到 files/<name>（时间戳前缀）+ logcat。任何异常静默。 */
    public static void appendLog(Context ctx, String name, String line) {
        try {
            File f = new File(ctx.getExternalFilesDir(null), name);
            FileOutputStream fo = new FileOutputStream(f, true);
            fo.write((ts() + " " + line + "\n").getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ig) { }
        Log.i(TAG, name + ": " + line);
    }

    // ---------------------------------------------------------------- 主干

    /** 跑一整轮；返回总结行（含通过/警告/失败计数）。重复触发时返回 "busy…"。 */
    public static String run(Context ctx, String mode, Progress cb) {
        synchronized (LOCK) {
            if (running) return "busy（已有测试在跑）";
            running = true;
        }
        long t0 = System.currentTimeMillis();
        String m = (mode == null || mode.isEmpty()) ? "all" : mode;
        boolean doData = !"render".equals(m);
        boolean doRender = !"quick".equals(m);
        int total = (doData ? 13 : 0) + (doRender ? 5 : 0);   // 渲染 4 页 + 图标/对话框预览 1 步
        List<String> out = new ArrayList<String>();
        File logFile = new File(ctx.getExternalFilesDir(null), "test_run.txt");
        try {
            FileOutputStream fo = new FileOutputStream(logFile, false);
            fo.write(("== vtouch 测试台 " + ts() + " mode=" + m + " ==\n").getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ig) { }

        int idx = 0;
        try {
            if (doData) {
                if (cb != null) cb.step("su 检查", ++idx, total);
                stepRoot(out, logFile);
                if (cb != null) cb.step("实时文件往返", ++idx, total);
                stepLiveRoundTrip(out, logFile);
                if (cb != null) cb.step("区域合成用例", ++idx, total);
                synthRegions(out, logFile);
                if (cb != null) cb.step("操作合成用例", ++idx, total);
                synthOps(out, logFile);
                if (cb != null) cb.step("校验与命名", ++idx, total);
                validateCases(out, logFile);
                if (cb != null) cb.step("写盘链路", ++idx, total);
                writeChain(out, logFile);
                if (cb != null) cb.step("跨方案复制", ++idx, total);
                crossCopyChecks(out, logFile);
                if (cb != null) cb.step("素材文件", ++idx, total);
                assetsChecks(out, logFile);
                if (cb != null) cb.step("匹配引擎", ++idx, total);
                matcherChecks(out, logFile);
                if (cb != null) cb.step("抓帧·帧库", ++idx, total);
                frameChecks(ctx, out, logFile);
                if (cb != null) cb.step("看图器", ++idx, total);
                imageChecks(ctx, out, logFile);
                if (cb != null) cb.step("会话页", ++idx, total);
                viewChecks(ctx, out, logFile);
                viewPickChecks(ctx, out, logFile);
                if (cb != null) cb.step("v7.1 修复轮", ++idx, total);
                fixBatchChecks(ctx, out, logFile);
            }
            if (doRender) {
                renderPages(ctx, out, logFile, cb, idx + 1, total);
                extraChecks(ctx, out, logFile, cb, idx + 5, total);
            }
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 测试台顶层异常: " + t);
        }

        int p = 0, f = 0, w = 0;
        for (String s : out) {
            if (s.startsWith("[OK]")) p++;
            else if (s.startsWith("[FAIL]")) f++;
            else if (s.startsWith("[WARN]")) w++;
        }
        String summary = "== 通过 " + p + " · 警告 " + w + " · 失败 " + f + "（mode=" + m + "，用时 "
                + String.format(Locale.US, "%.1f", (System.currentTimeMillis() - t0) / 1000.0) + "s）==";
        rec(out, logFile, summary);
        try {
            StringBuilder full = new StringBuilder();
            for (String s : out) full.append(s).append('\n');
            FileOutputStream fo = new FileOutputStream(new File(ctx.getExternalFilesDir(null), "test_report.txt"));
            fo.write(full.toString().getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ig) { }
        synchronized (LOCK) { running = false; }
        return summary;
    }

    // ---------------------------------------------------------------- 数据自检（供 App 内「自检」按钮复用）

    /** su + 两份文件往返一致性 + 方案概况（原 MainActivity.selfTest 逻辑，报告文本）。 */
    public static String dataSelfTest() {
        StringBuilder sb = new StringBuilder();
        sb.append("== vtouch 自检 ==\n");
        Su.R id = Su.run("id");
        boolean root = id.ok() && id.out.contains("uid=0");
        sb.append(root ? "[OK] su: root 可用\n" : "[FAIL] su: " + id.trimmed() + "\n");

        String rt = Su.read(Regions.PATH);
        if (rt == null) {
            sb.append("[FAIL] regions.conf 读取失败\n");
        } else {
            try {
                Regions.File f1 = Regions.File.parse(rt);
                Regions.File f2 = Regions.File.parse(f1.serialize());
                boolean same = regionsEqual(f1, f2);
                sb.append(same ? "[OK] " : "[FAIL] ")
                  .append("regions.conf: ").append(f1.regions.size()).append(" 区域，往返一致=").append(same).append('\n');
            } catch (Exception e) {
                sb.append("[FAIL] regions.conf 解析: ").append(e).append('\n');
            }
        }

        String ot = Su.read(Ops.PATH);
        if (ot == null) {
            sb.append("[FAIL] ops.conf 读取失败\n");
        } else {
            try {
                Ops.File f1 = Ops.File.parse(ot);
                Ops.File f2 = Ops.File.parse(f1.serialize());
                boolean same = opsEqual(f1, f2);
                int steps = 0;
                for (Ops.Op o : f1.ops) steps += o.steps.size();
                sb.append(same ? "[OK] " : "[FAIL] ")
                  .append("ops.conf: v").append(f1.version).append(' ').append(f1.ops.size())
                  .append(" 操作 / ").append(steps).append(" 步骤，往返一致=").append(same).append('\n');
            } catch (Exception e) {
                sb.append("[FAIL] ops.conf 解析: ").append(e).append('\n');
            }
        }

        sb.append("schemes: ").append(Schemes.list().size()).append(" 个，当前 ")
          .append(Schemes.current()).append('\n');
        String pid = Su.corePid();
        sb.append("核心: ").append(pid.isEmpty() ? "未运行" : ("运行中 pid " + pid)).append('\n');
        return sb.toString();
    }

    // ---------------------------------------------------------------- 各步

    private static void stepRoot(List<String> out, File logFile) {
        try {
            Su.R id = Su.run("id");
            boolean root = id.ok() && id.out.contains("uid=0");
            rec(out, logFile, (root ? "[OK] " : "[FAIL] ") + "su root：" + (root ? "可用" : id.trimmed()));
            String pid = Su.corePid();
            rec(out, logFile, "[INFO] 核心：" + (pid.isEmpty() ? "未运行" : "运行中 pid " + pid));
            rec(out, logFile, "[INFO] 方案：" + Schemes.list().size() + " 个，当前 " + Schemes.current());
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] su 检查异常: " + t);
        }
    }

    private static void stepLiveRoundTrip(List<String> out, File logFile) {
        String rt = Su.read(Regions.PATH);
        if (rt == null) {
            rec(out, logFile, "[FAIL] regions.conf 读取失败");
        } else {
            try {
                Regions.File f1 = Regions.File.parse(rt);
                Regions.File f2 = Regions.File.parse(f1.serialize());
                boolean same = regionsEqual(f1, f2);
                rec(out, logFile, (same ? "[OK] " : "[FAIL] ") + "实时 regions.conf：" + f1.regions.size()
                        + " 区域，往返一致=" + same);
            } catch (Exception e) {
                rec(out, logFile, "[FAIL] 实时 regions.conf 解析: " + e);
            }
        }
        String ot = Su.read(Ops.PATH);
        if (ot == null) {
            rec(out, logFile, "[FAIL] ops.conf 读取失败");
        } else {
            try {
                Ops.File f1 = Ops.File.parse(ot);
                Ops.File f2 = Ops.File.parse(f1.serialize());
                boolean same = opsEqual(f1, f2);
                int steps = 0;
                for (Ops.Op o : f1.ops) steps += o.steps.size();
                rec(out, logFile, (same ? "[OK] " : "[FAIL] ") + "实时 ops.conf：v" + f1.version + " "
                        + f1.ops.size() + " 操作 / " + steps + " 步骤，往返一致=" + same);
            } catch (Exception e) {
                rec(out, logFile, "[FAIL] 实时 ops.conf 解析: " + e);
            }
        }
    }

    private static void synthRegions(List<String> out, File logFile) {
        try {
            String sample = "#vtouch-regions v2\n#frame 0 1440 3168\n"
                    + "region r1 0 10 20 30 40 1\nregion c1 1 100 100 50 0 1\n"
                    + "bind r1 op1 press\nbind c1 - down\nkind r1 1\nkind c1 0\nhide r2\n"
                    + "某未知行 x y\nbind ghost opX down\n";
            Regions.File f1 = Regions.File.parse(sample);
            Regions.File f2 = Regions.File.parse(f1.serialize());
            boolean eq = regionsEqual(f1, f2);
            Regions.Region r1 = f1.find("r1");
            Regions.Region c1 = f1.find("c1");
            boolean fields = f1.regions.size() == 2 && r1 != null && c1 != null
                    && "op1".equals(r1.bindOp) && "press".equals(r1.bindEv) && r1.kind == 1
                    && c1.kind == 0 && f1.hideIds.size() == 1 && "r2".equals(f1.hideIds.get(0))
                    && f1.unknown.size() == 2;
            rec(out, logFile, (eq && fields ? "[OK] " : "[FAIL] ")
                    + "区域合成：2 区域/绑定/开关/隐藏/未知行保留，往返一致=" + eq + " 字段=" + fields);
            boolean threw1 = false;
            try { Regions.File.parse("#vtouch-regions v3\n"); } catch (Exception ex) { threw1 = true; }
            boolean threw2 = false;
            try { Regions.File.parse("region r1 0 0 0 0 0 1\n"); } catch (Exception ex) { threw2 = true; }
            rec(out, logFile, (threw1 && threw2 ? "[OK] " : "[FAIL] ")
                    + "区域负例：v3 拒绝=" + threw1 + " 缺版本行拒绝=" + threw2);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 区域合成用例异常: " + t);
        }
    }

    private static void synthOps(List<String> out, File logFile) {
        try {
            String s5 = "#vtouch-ops v5\nop op1 gate - autooff 1\n"
                    + "step 1 10 20 0 0 50 - 0 0 -\n"
                    + "step 9 0 0 0 0 0 dx 0 0 tdx + 10\n"
                    + "step 10 8 1 0 2 0 tpl1 0 0 area1\n"
                    + "step 11 1 0 0 2 0 pts1 0 0 -\n"
                    + "step 2 -10 20 -11 40 300 - 100 -12 -\n"
                    + "step 12 1 2 3 4 5 r 7 8 x y z\n"
                    + "op op2 gate c1 autooff 0 extraF1 extraF2\n"
                    + "step 1 1 2 0 0 10 - 0 0 -\n"
                    + "垃圾行 keep me\n";
            Ops.File f5 = Ops.File.parse(s5);
            Ops.Op o1 = f5.find("op1");
            Ops.Op o2 = f5.find("op2");
            boolean c1 = f5.version == 5 && f5.ops.size() == 2;
            boolean c2 = o1 != null && o1.steps.size() == 6;
            Ops.Step calc = c2 ? o1.steps.get(1) : null;
            Ops.Step fi = c2 ? o1.steps.get(2) : null;
            Ops.Step fc = c2 ? o1.steps.get(3) : null;
            Ops.Step sw = c2 ? o1.steps.get(4) : null;
            boolean c3 = calc != null && calc.type == Ops.T_CALC && calc.a1 == 0
                    && "dx".equals(calc.ref) && "tdx + 10".equals(calc.expr);
            boolean c4 = fi != null && fi.type == Ops.T_FINDIMAGE && fi.a1 == 8 && fi.a2 == 1
                    && "tpl1".equals(fi.ref) && "area1".equals(fi.expr) && fi.a4 == Ops.C_CONT;
            boolean c5 = fc != null && fc.type == Ops.T_FINDCOLOR && fc.a1 == 1 && "pts1".equals(fc.ref);
            boolean c6 = sw != null && sw.j1 == 100 && sw.j2 == -12 && sw.a1 == -10;
            boolean c7 = c2 && o1.steps.get(5).raw != null;
            boolean c8 = o2 != null && "extraF1 extraF2".equals(o2.extra) && f5.unknown.size() == 1;
            boolean rt1 = opsEqual(f5, Ops.File.parse(f5.serialize()));
            rec(out, logFile, (c1 && c2 && c3 && c4 && c5 && c6 && c7 && c8 && rt1 ? "[OK] " : "[FAIL] ")
                    + "操作合成 v5：计算/找图/找色/滑动变量/未知逐字，往返一致=" + rt1);

            String s4 = "#vtouch-ops v4\nop opT gate - autooff 0\n"
                    + "step 9 1 0 0 0 0 - 0 0 tdx * 2\n"
                    + "step 1 -6 20 0 0 50 - 0 0 -\n";
            Ops.File f4 = Ops.File.parse(s4);
            Ops.Op oT = f4.find("opT");
            boolean t1 = f4.version == 5 && oT != null && oT.steps.size() == 2
                    && oT.steps.get(0).a1 == 0 && "r1".equals(oT.steps.get(0).ref)
                    && oT.steps.get(1).a1 == -10;
            boolean rt2 = opsEqual(f4, Ops.File.parse(f4.serialize()));
            rec(out, logFile, (t1 && rt2 ? "[OK] " : "[FAIL] ")
                    + "操作合成 v4→v5 翻译：槽定名 r1/字段 -6→-10 重映射=" + t1 + " 往返一致=" + rt2);

            String s3 = "#vtouch-ops v3\nop opX gate - autooff 0\nstep 1 1 2 3 4 5 - 6 7\n";
            Ops.File f3 = Ops.File.parse(s3);
            boolean c5b = f3.version == 5 && opsEqual(f3, Ops.File.parse(f3.serialize()));
            boolean threw = false;
            try { Ops.File.parse("op x gate - autooff 0\n"); } catch (Exception ex) { threw = true; }
            rec(out, logFile, (c5b && threw ? "[OK] " : "[FAIL] ")
                    + "操作合成 v3：9 字段兼容读 往返一致=" + c5b + " 缺版本行拒绝=" + threw);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 操作合成用例异常: " + t);
        }
    }

    private static void validateCases(List<String> out, File logFile) {
        try {
            Regions.File rf = new Regions.File();
            Regions.Region r1 = new Regions.Region("r1");
            r1.type = 0; r1.a1 = 0; r1.a2 = 0; r1.a3 = 100; r1.a4 = 100;
            rf.regions.add(r1);
            boolean v1 = Regions.validate(r1, rf, r1) == null;
            Regions.Region rc = new Regions.Region("c1");
            rc.type = 1; rc.a1 = 1; rc.a2 = 1; rc.a3 = -5; rc.a4 = 0;
            boolean v2 = Regions.validate(rc, rf, null) != null;
            Regions.Region dup = new Regions.Region("r1");
            dup.a3 = 1; dup.a4 = 1;
            boolean v3 = Regions.validate(dup, rf, null) != null;
            Regions.Region oob = new Regions.Region("r9");
            oob.a3 = 4097; oob.a4 = 0;
            boolean v4 = Regions.validate(oob, rf, null) != null;
            boolean n1 = Regions.idOk("r1") && Regions.idOk("a_b-C9")
                    && !Regions.idOk("-") && !Regions.idOk("")
                    && !Regions.idOk("0123456789abcdefg") && !Regions.idOk("a.b")
                    && Regions.idOk("0123456789abcde");

            Ops.File of = new Ops.File();
            of.version = 5;
            Ops.Op ok = new Ops.Op();
            ok.name = "opk";
            Ops.Step oks = new Ops.Step();
            oks.type = Ops.T_TAP; oks.a1 = 5; oks.a2 = 5; oks.ms = 50;
            ok.steps.add(oks);
            of.ops.add(ok);
            boolean v5 = Ops.validate(ok, of, ok) == null;
            Ops.Op bad = new Ops.Op();
            bad.name = "opb";
            Ops.Step js = new Ops.Step();
            js.type = Ops.T_JUMP; js.a1 = 2;
            bad.steps.add(js);
            boolean v6 = Ops.validate(bad, of, null) != null;
            Ops.Op bad2 = new Ops.Op();
            bad2.name = "opc";
            Ops.Step cs = new Ops.Step();
            cs.type = Ops.T_COND_REGION; cs.ref = "c1"; cs.a3 = 0; cs.a4 = Ops.C_JUMP; cs.j1 = 3;
            bad2.steps.add(cs);
            boolean v7 = Ops.validate(bad2, of, null) != null;

            Ops.Op okCalc = new Ops.Op();
            okCalc.name = "opd";
            Ops.Step cs9 = new Ops.Step();
            cs9.type = Ops.T_CALC; cs9.a1 = 0; cs9.ref = "dx"; cs9.expr = "tdx + 1";
            okCalc.steps.add(cs9);
            boolean v8 = Ops.validate(okCalc, of, null) == null;
            Ops.Op badCalc = new Ops.Op();
            badCalc.name = "ope";
            Ops.Step cs9b = new Ops.Step();
            cs9b.type = Ops.T_CALC; cs9b.a1 = 0; cs9b.ref = "dx"; cs9b.expr = "tdx +";
            badCalc.steps.add(cs9b);
            boolean v9 = Ops.validate(badCalc, of, null) != null;
            Ops.Op badCalc2 = new Ops.Op();
            badCalc2.name = "oph";
            Ops.Step cs9c = new Ops.Step();
            cs9c.type = Ops.T_CALC; cs9c.a1 = 0; cs9c.ref = "dx"; cs9c.expr = "foo + 1";
            badCalc2.steps.add(cs9c);
            boolean v9b = Ops.validate(badCalc2, of, null) != null;
            Ops.Op badFi = new Ops.Op();
            badFi.name = "opf";
            Ops.Step fi9 = new Ops.Step();
            fi9.type = Ops.T_FINDIMAGE; fi9.a1 = 999; fi9.ref = "t1"; fi9.a4 = Ops.C_CONT;
            badFi.steps.add(fi9);
            boolean v10 = Ops.validate(badFi, of, null) != null;
            Ops.Op badFc = new Ops.Op();
            badFc.name = "opg";
            Ops.Step fc9 = new Ops.Step();
            fc9.type = Ops.T_FINDCOLOR; fc9.a1 = 0; fc9.ref = "p1"; fc9.a4 = Ops.C_CONT;
            badFc.steps.add(fc9);
            boolean v11 = Ops.validate(badFc, of, null) != null;
            Ops.Op badExpr = new Ops.Op();
            badExpr.name = "opi";
            Ops.Step be = new Ops.Step();
            be.type = Ops.T_TAP; be.a1 = 5; be.a2 = 5; be.ms = 50; be.expr = "tdx";
            badExpr.steps.add(be);
            boolean v12 = Ops.validate(badExpr, of, null) != null;

            boolean all = v1 && v2 && v3 && v4 && v5 && v6 && v7 && v8 && v9 && v9b && v10 && v11 && v12 && n1;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "校验与命名："
                    + "矩形合法=" + v1 + " 半径负拒=" + v2 + " 重名拒=" + v3 + " 超界拒=" + v4
                    + " 操作合法=" + v5 + " 跳转越界拒=" + v6 + " 条件跳转越界拒=" + v7
                    + " 计算合法=" + v8 + " 表达式语法拒=" + v9 + " 未知名拒=" + v9b
                    + " 找图阈值拒=" + v10 + " 找色单点带点集拒=" + v11 + " expr挂错类型拒=" + v12
                    + " 命名规则=" + n1);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 校验用例异常: " + t);
        }
    }

    private static void writeChain(List<String> out, File logFile) {
        try {
            String d = "/data/local/tmp/vte-selftest";
            Su.run("rm -rf '" + d + "' && mkdir -p '" + d + "'");
            byte[] a = "内容A α-β\nline2\n".getBytes("UTF-8");
            byte[] b = "内容B\n".getBytes("UTF-8");
            Su.R w1 = Su.writeAtomic(d + "/t.conf", a);
            String s1 = Su.read(d + "/t.conf");
            boolean ok1 = w1.ok() && s1 != null && s1.equals(new String(a, "UTF-8"));
            Su.backup(d + "/t.conf");
            Su.R w2 = Su.writeAtomic(d + "/t.conf", b);
            String s2 = Su.read(d + "/t.conf");
            String bak = Su.read(d + "/t.conf.bak");
            boolean ok2 = w2.ok() && s2 != null && s2.equals(new String(b, "UTF-8"))
                    && bak != null && bak.equals(new String(a, "UTF-8"));
            Su.run("rm -rf '" + d + "'");
            rec(out, logFile, (ok1 && ok2 ? "[OK] " : "[FAIL] ")
                    + "写盘链路（scratch）：原子写=" + ok1 + " 备份保留=" + ok2);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 写盘链路异常: " + t);
        }
    }

    // ---------------------------------------------------------------- 跨方案复制（v14）

    /** 跨方案复制：纯逻辑三场景 —— 无冲突直带 / 冲突改名+引用改写 / 覆盖替换；含「目标已有依赖不上带」与基帧警告。 */
    private static void crossCopyChecks(List<String> out, File logFile) {
        try {
            final String srcR = "#vtouch-regions v2\n#frame 0 1440 3168\n"
                    + "region r3 0 100 100 200 200 1\n"
                    + "bind r3 挂机 down\n"
                    + "kind r3 1\n"
                    + "region s1 1 720 800 50 0 1\n"
                    + "bind s1 挂机 down\n"
                    + "kind s1 1\n";
            final String srcO = "#vtouch-ops v5\n"
                    + "op 挂机 gate s1 autooff 0\n"
                    + "step 6 0 0 0 2 0 r3 0 0 -\n"
                    + "step 1 300 400 0 0 50 - 0 0 -\n";
            final String eR = "#vtouch-regions v2\n#frame 0 1440 3168\n";
            final String eO = "#vtouch-ops v5\n";
            final String tR = "#vtouch-regions v2\n#frame 0 1440 3168\n"
                    + "region s1 1 555 666 44 0 1\n"
                    + "bind s1 - down\n"
                    + "kind s1 1\n";
            final String tO = "#vtouch-ops v5\n"
                    + "op 挂机 gate - autooff 0\n"
                    + "step 3 0 0 0 0 100 - 0 0 -\n";

            /* 用例1：复制区域、目标为空 —— 无冲突直带（递归依赖：r3 → 挂机 → s1） */
            CrossCopy.Inspect i1 = CrossCopy.inspect(true, "r3", srcR, srcO, eR, eO);
            boolean c1 = i1.err == null && !i1.conflict && i1.carries.size() == 2
                    && i1.carries.contains("操作 挂机") && i1.carries.contains("区域 s1");
            CrossCopy.Rendered d1 = CrossCopy.render(i1, false);
            boolean r1ok = false;
            try {
                Regions.File g1 = Regions.File.parse(d1.newDstRegions);
                Ops.File o1 = Ops.File.parse(d1.newDstOps);
                Regions.Region nr3 = g1.find("r3"), ns1 = g1.find("s1");
                Ops.Op no = o1.find("挂机");
                r1ok = nr3 != null && "挂机".equals(nr3.bindOp) && nr3.kind == 1 && nr3.a1 == 100 && nr3.a3 == 200
                        && ns1 != null && "挂机".equals(ns1.bindOp)
                        && no != null && "s1".equals(no.gate) && no.steps.size() == 2 && "r3".equals(no.steps.get(0).ref);
            } catch (Throwable ig) { }

            /* 用例2：复制操作、目标已有同名「挂机」+ 已有依赖 s1 —— 冲突改名、r3 上带并改写引用、s1 不上带 */
            CrossCopy.Inspect i2 = CrossCopy.inspect(false, "挂机", srcR, srcO, tR, tO);
            boolean c2 = i2.err == null && i2.conflict && "挂机-2".equals(i2.renamePreview)
                    && i2.carries.size() == 1 && i2.carries.contains("区域 r3");
            CrossCopy.Rendered d2 = CrossCopy.render(i2, true);
            boolean r2ok = false;
            try {
                Regions.File g2 = Regions.File.parse(d2.newDstRegions);
                Ops.File o2 = Ops.File.parse(d2.newDstOps);
                Regions.Region ns1 = g2.find("s1"), nr3 = g2.find("r3");
                Ops.Op old = o2.find("挂机"), nw = o2.find("挂机-2");
                r2ok = ns1 != null && ns1.a1 == 555 && "-".equals(ns1.bindOp)          /* 目标已有 s1 原样保留 */
                        && nr3 != null && "挂机-2".equals(nr3.bindOp)                  /* 上带的 r3 改写指向新名 */
                        && "挂机-2".equals(d2.appliedName)
                        && old != null && old.steps.size() == 1                        /* 目标旧「挂机」保留 */
                        && nw != null && "s1".equals(nw.gate) && nw.steps.size() == 2 && "r3".equals(nw.steps.get(0).ref);
            } catch (Throwable ig) { }

            /* 用例3：同场景覆盖模式 —— 目标「挂机」被替换（原名），r3 上带且引用不改写 */
            CrossCopy.Inspect i3 = CrossCopy.inspect(false, "挂机", srcR, srcO, tR, tO);
            CrossCopy.Rendered d3 = CrossCopy.render(i3, false);
            boolean r3ok = false;
            try {
                Regions.File g3 = Regions.File.parse(d3.newDstRegions);
                Ops.File o3 = Ops.File.parse(d3.newDstOps);
                Ops.Op ov = o3.find("挂机");
                Regions.Region nr3b = g3.find("r3");
                r3ok = ov != null && ov.steps.size() == 2 && "s1".equals(ov.gate) && o3.find("挂机-2") == null
                        && nr3b != null && "挂机".equals(nr3b.bindOp);
            } catch (Throwable ig) { }

            /* 用例4：基帧不同（目标横屏）→ 出提示 */
            CrossCopy.Inspect i4 = CrossCopy.inspect(true, "r3", srcR, srcO, "#vtouch-regions v2\n#frame 1 3168 1440\n", eO);
            boolean c4 = i4.err == null && i4.frameWarn != null;

            boolean all = c1 && r1ok && c2 && r2ok && r3ok && c4;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "跨方案复制："
                    + "无冲突直带=" + (c1 && r1ok) + " 冲突改名+改写=" + (c2 && r2ok)
                    + " 覆盖替换=" + r3ok + " 基帧警告=" + c4);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 跨方案复制异常: " + t);
        }
    }

    // ---------------------------------------------------------------- 素材文件（v7 T1）

    /** 素材层检查：金样/往返/旧尾/坏文件/命名/pts/refs/su 读写改删（scratch）。 */
    private static void assetsChecks(List<String> out, File logFile) {
        Assets.Tmpl t = new Assets.Tmpl();
        t.w = 2; t.h = 2; t.rot = 0; t.x = 5; t.y = 6;
        t.avgRgb = (7 << 16) | (8 << 8) | 9; t.hasXy = true; t.gray = new byte[]{1, 2, 3, 4};
        byte[] b = null;
        try {
            b = Assets.bytesTmpl(t);
            String want = "56544d31" + "01000000" + "0200" + "0200" + "00" + "00"
                    + "01020304" + "0500" + "0600" + "070809" + "00";
            String got = hex(b);
            rec(out, logFile, (want.equals(got) ? "[OK] " : "[FAIL] ") + "素材金样 tmpl 26B：" + got);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材金样异常: " + e);
        }
        try {
            Assets.Tmpl r = Assets.parseTmpl(b);
            boolean rt = r != null && r.w == 2 && r.h == 2 && r.rot == 0 && r.x == 5 && r.y == 6
                    && r.hasXy && r.avgRgb == ((7 << 16) | (8 << 8) | 9) && eqArr(r.gray, t.gray);
            rec(out, logFile, (rt ? "[OK] " : "[FAIL] ") + "素材 tmpl 往返=" + rt);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材往返异常: " + e);
        }
        try {
            byte[] leg = new byte[b.length - 4];
            System.arraycopy(b, 0, leg, 0, leg.length);
            Assets.Tmpl lr = Assets.parseTmpl(leg);
            boolean lk = lr != null && lr.hasXy && lr.avgRgb == -1 && lr.x == 5 && lr.y == 6;
            byte[] badMg = b.clone(); badMg[0] = 'X';
            byte[] badVer = b.clone(); badVer[4] = 2;
            byte[] trc = new byte[10]; System.arraycopy(b, 0, trc, 0, 10);
            boolean bd = Assets.parseTmpl(null) == null && Assets.parseTmpl(badMg) == null
                    && Assets.parseTmpl(badVer) == null && Assets.parseTmpl(trc) == null;
            rec(out, logFile, (lk && bd ? "[OK] " : "[FAIL] ") + "素材旧4B尾=" + lk + " 坏文件拒=" + bd);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材旧尾/坏文件异常: " + e);
        }
        try {
            boolean nm = Assets.nameOk("a") && Assets.nameOk("A-Z_09-x") && Assets.nameOk("abcdefghijklmno")
                    && Assets.nameOk("有中文") && Assets.nameOk("中文名")
                    && !Assets.nameOk("") && !Assets.nameOk("-") && !Assets.nameOk("a.b")
                    && !Assets.nameOk("abcdefghijklmnop") && !Assets.nameOk("一二三四五六七八九十百千万亿零甲")
                    && !Assets.nameOk("a b");
            rec(out, logFile, (nm ? "[OK] " : "[FAIL] ") + "素材命名规则=" + nm);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材命名异常: " + e);
        }
        try {
            Assets.Pts p = new Assets.Pts();
            p.baseRgb = 0x112233; p.baseTol = 12; p.x = 100; p.y = 200; p.hasXy = true;
            p.pts = new Assets.Pt[16];
            for (int i = 0; i < 16; i++) {
                Assets.Pt q = new Assets.Pt();
                q.dx = i - 8; q.dy = 8 - i; q.rgb = 0x010203 + i; q.tol = 12;
                p.pts[i] = q;
            }
            Assets.Pts pr = Assets.parsePts(Assets.bytesPts(p));
            boolean pk = pr != null && pr.baseRgb == 0x112233 && pr.baseTol == 12 && pr.hasXy
                    && pr.x == 100 && pr.y == 200 && pr.pts != null && pr.pts.length == 16;
            if (pk) {
                for (int i = 0; i < 16 && pk; i++) {
                    pk = pr.pts[i].dx == i - 8 && pr.pts[i].dy == 8 - i
                            && pr.pts[i].rgb == (0x010203 + i) && pr.pts[i].tol == 12;
                }
            }
            Assets.Pts over = new Assets.Pts();
            over.baseRgb = 1; over.baseTol = 0; over.x = 0; over.y = 0; over.hasXy = true;
            over.pts = new Assets.Pt[17];
            for (int i = 0; i < 17; i++) { Assets.Pt q = new Assets.Pt(); over.pts[i] = q; }
            boolean pv = Assets.bytesPts(over) == null;
            rec(out, logFile, (pk && pv ? "[OK] " : "[FAIL] ") + "素材 pts 往返(负偏移/16)=" + pk + " 17点拒=" + pv);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材 pts 异常: " + e);
        }
        try {
            List<String> texts = new ArrayList<String>();
            texts.add("#vtouch-ops v5\nop opA gate - autooff 0\n"
                    + "step 10 8 1 0 2 0 tplX 0 0 -\n"
                    + "step 11 1 0 0 2 0 ptsY 0 0 -\n"
                    + "step 11 0 8 0 2 0 - 0 0 -\n");
            List<String> labels = new ArrayList<String>();
            labels.add("当前");
            List<String> r1 = Assets.refsFromTexts(texts, labels, "tplX");
            List<String> r2 = Assets.refsFromTexts(texts, labels, "ptsY");
            List<String> r3 = Assets.refsFromTexts(texts, labels, "none");
            boolean rk = r1.size() == 1 && r1.get(0).contains("opA")
                    && r2.size() == 1 && r2.get(0).contains("opA") && r3.isEmpty();
            rec(out, logFile, (rk ? "[OK] " : "[FAIL] ") + "素材引用扫描=" + rk
                    + "（tplX " + r1.size() + " / ptsY " + r2.size() + " / none " + r3.size() + "）");
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材引用异常: " + e);
        }
        String sd = "/data/local/tmp/vte-selftest-assets";
        String old = Assets.dir;
        try {
            Su.run("rm -rf '" + sd + "' && mkdir -p '" + sd + "'");
            Assets.dir = sd;
            Assets.Tmpl t2 = new Assets.Tmpl();
            t2.w = 3; t2.h = 1; t2.rot = 0; t2.x = 9; t2.y = 10; t2.avgRgb = 0x0A0B0C;
            t2.hasXy = true; t2.gray = new byte[]{9, 8, 7};
            Assets.writeTmpl("t_one", t2);
            boolean w1 = Assets.exists("t_one");
            Assets.Tmpl rd = Assets.readTmpl("t_one");
            boolean w2 = rd != null && rd.w == 3 && rd.h == 1 && eqArr(rd.gray, t2.gray)
                    && rd.avgRgb == 0x0A0B0C && rd.hasXy && rd.x == 9;
            Assets.Pts pp = new Assets.Pts();
            pp.baseRgb = 0x123456; pp.baseTol = 5; pp.x = 7; pp.y = 8; pp.hasXy = true;
            Assets.Pt q1 = new Assets.Pt(); q1.dx = -3; q1.dy = 4; q1.rgb = 0x111213; q1.tol = 5;
            pp.pts = new Assets.Pt[]{q1};
            Assets.writePts("p_one", pp);
            Assets.Pts prd = Assets.readPts("p_one");
            boolean w3 = prd != null && prd.baseRgb == 0x123456 && prd.baseTol == 5
                    && prd.pts != null && prd.pts.length == 1 && prd.pts[0].dx == -3
                    && prd.pts[0].dy == 4 && prd.hasXy && prd.x == 7 && prd.y == 8;
            Assets.rename("t_one", "t_two", false);
            boolean w5 = Assets.exists("t_two") && !Assets.exists("t_one");
            boolean w6 = Assets.delete("t_two") && !Assets.exists("t_two");
            Assets.writeTmpl("坏 名", t2);
            boolean w7 = !Assets.exists("坏 名");
            rec(out, logFile, (w1 && w2 && w3 && w5 && w6 && w7 ? "[OK] " : "[FAIL] ")
                    + "素材读写链(scratch)：写=" + w1 + " 读=" + w2 + " pts=" + w3
                    + " 改名=" + w5 + " 删=" + w6 + " 拒非法名=" + w7);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 素材读写链异常: " + e);
        } finally {
            Su.run("rm -rf '" + sd + "'");
            Assets.dir = old;
        }
    }

    private static boolean eqArr(byte[] a, byte[] b) {
        if (a == null || b == null) return a == b;
        if (a.length != b.length) return false;
        for (int i = 0; i < a.length; i++) if (a[i] != b[i]) return false;
        return true;
    }

    private static String hex(byte[] b) {
        if (b == null) return "(null)";
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x & 0xFF));
        return sb.toString();
    }

    // ---------------------------------------------------------------- 匹配引擎（v7 T2）

    /** 引擎直译检查：grayPx / 映射表 / 找色（单点/多点）/ 找图（直搜/金字塔/边界）/ 颜色校验 / 性能冒烟。 */
    private static void matcherChecks(List<String> out, File logFile) {
        // 1) grayPx 常数
        try {
            boolean g = (Matcher.grayPx(0, 0, 0) & 0xFF) == 0
                    && (Matcher.grayPx(255, 255, 255) & 0xFF) == 255
                    && (Matcher.grayPx(100, 150, 200) & 0xFF) == 141;
            rec(out, logFile, (g ? "[OK] " : "[FAIL] ") + "匹配 grayPx 常数=" + g);
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配 grayPx 异常: " + e); }

        // 2) 映射表（rot0..3 角点 + 矩形换算）
        try {
            int[] a = new int[2];
            Matcher.frameToLogic(0, 100, 200, 10, 20, a);
            boolean m0 = a[0] == 10 && a[1] == 20;
            Matcher.frameToLogic(1, 200, 100, 0, 0, a);     boolean m1a = a[0] == 99 && a[1] == 0;
            Matcher.frameToLogic(1, 200, 100, 0, 99, a);    boolean m1b = a[0] == 0 && a[1] == 0;
            Matcher.frameToLogic(1, 200, 100, 199, 99, a);  boolean m1c = a[0] == 0 && a[1] == 199;
            Matcher.frameToLogic(1, 200, 100, 199, 0, a);   boolean m1d = a[0] == 99 && a[1] == 199;
            Matcher.frameToLogic(2, 100, 200, 0, 0, a);     boolean m2a = a[0] == 99 && a[1] == 199;
            Matcher.frameToLogic(2, 100, 200, 99, 199, a);  boolean m2b = a[0] == 0 && a[1] == 0;
            Matcher.frameToLogic(3, 200, 100, 0, 0, a);     boolean m3a = a[0] == 0 && a[1] == 199;
            Matcher.frameToLogic(3, 200, 100, 199, 99, a);  boolean m3b = a[0] == 99 && a[1] == 0;
            boolean mm = m0 && m1a && m1b && m1c && m1d && m2a && m2b && m3a && m3b;
            int[] rc1 = Matcher.logicRectToFrame(1, 200, 100, 10, 20, 30, 40);
            boolean mr1 = rc1[0] == 20 && rc1[1] == 60 && rc1[2] == 40 && rc1[3] == 30;
            int[] rc2 = Matcher.logicRectToFrame(2, 100, 200, 10, 20, 30, 40);
            boolean mr2 = rc2[0] == 60 && rc2[1] == 140 && rc2[2] == 30 && rc2[3] == 40;
            rec(out, logFile, (mm && mr1 && mr2 ? "[OK] " : "[FAIL] ")
                    + "匹配 映射表 rot0..3=" + mm + " 矩形换算=" + (mr1 && mr2));
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配映射表异常: " + e); }

        // 3) 找色：命中 / 行优先 / 容差边界 / 区域裁剪 / 空交集
        try {
            byte[] rgba = rgbaSolid(40, 40, 10, 10, 10);
            rgbaSet(rgba, 40, 5, 7, 200, 30, 40);
            Matcher.Frame f = Matcher.prepare(rgba, 40, 40, 0);
            Matcher.Hit h1 = Matcher.findColor(f, 0, 0, 40, 40, (200 << 16) | (30 << 8) | 40, 0);
            boolean c1 = h1 != null && h1.x == 5 && h1.y == 7;
            rgbaSet(rgba, 40, 2, 2, 200, 30, 40);
            f = Matcher.prepare(rgba, 40, 40, 0);
            Matcher.Hit h2 = Matcher.findColor(f, 0, 0, 40, 40, (200 << 16) | (30 << 8) | 40, 0);
            boolean c2 = h2 != null && h2.x == 2 && h2.y == 2;
            rgbaSet(rgba, 40, 20, 20, 150, 60, 60);
            f = Matcher.prepare(rgba, 40, 40, 0);
            Matcher.Hit h3 = Matcher.findColor(f, 20, 20, 5, 5, (150 << 16) | (50 << 8) | 50, 10);
            boolean c3 = h3 != null && h3.x == 20 && h3.y == 20;
            Matcher.Hit h4 = Matcher.findColor(f, 20, 20, 5, 5, (150 << 16) | (50 << 8) | 50, 9);
            boolean c4 = h4 == null;
            Matcher.Hit h5 = Matcher.findColor(f, 30, 30, 5, 5, (150 << 16) | (50 << 8) | 50, 10);
            boolean c5 = h5 == null;
            Matcher.Hit h6 = Matcher.findColor(f, -100, -100, 50, 50, 0, 0);
            boolean c6 = h6 == null;
            rec(out, logFile, (c1 && c2 && c3 && c4 && c5 && c6 ? "[OK] " : "[FAIL] ")
                    + "匹配 找色：命中=" + c1 + " 行优先=" + c2 + " 容差界=" + (c3 && c4)
                    + " 区域裁剪=" + c5 + " 空交集=" + c6);
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配找色异常: " + e); }

        // 4) 找色多点：假候选否决后命中 / 16 点 / 参考点越界否决
        try {
            byte[] rgba = rgbaSolid(40, 40, 10, 10, 10);
            rgbaSet(rgba, 40, 5, 5, 200, 0, 0);
            rgbaSet(rgba, 40, 30, 30, 200, 0, 0);
            rgbaSet(rgba, 40, 35, 30, 250, 250, 250);
            Matcher.Frame f = Matcher.prepare(rgba, 40, 40, 0);
            Matcher.Pt[] pts = new Matcher.Pt[1];
            pts[0] = new Matcher.Pt();
            pts[0].dx = 5; pts[0].dy = 0; pts[0].rgb = 0xFAFAFA; pts[0].tol = 0;
            Matcher.Hit h = Matcher.findColorMulti(f, 0, 0, 40, 40, 0xC80000, 0, pts, 1);
            boolean k1 = h != null && h.x == 30 && h.y == 30;
            Matcher.Pt[] p16 = new Matcher.Pt[16];
            for (int i = 0; i < 16; i++) {
                p16[i] = new Matcher.Pt();
                p16[i].rgb = 0xC80000; p16[i].tol = 0;
            }
            Matcher.Hit h16 = Matcher.findColorMulti(f, 0, 0, 40, 40, 0xC80000, 0, p16, 16);
            boolean k2 = h16 != null;
            Matcher.Pt[] po = new Matcher.Pt[1];
            po[0] = new Matcher.Pt();
            po[0].dx = 15; po[0].dy = 0; po[0].rgb = 0xC80000; po[0].tol = 0;
            Matcher.Hit ho = Matcher.findColorMulti(f, 0, 0, 40, 40, 0xC80000, 0, po, 1);
            boolean k3 = ho == null;
            rec(out, logFile, (k1 && k2 && k3 ? "[OK] " : "[FAIL] ")
                    + "匹配 找色多点：候选否决后命中=" + k1 + " 16点=" + k2 + " 越界否决=" + k3);
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配找色多点异常: " + e); }

        // 5) 找图：直搜(<8) / 金字塔(>=8) / 对齐偏移 / thresh 边界 / SAD==limit 含等号
        try {
            byte[] rgba = rgbaSolid(32, 32, 10, 10, 10);
            rgbaFillRect(rgba, 32, 10, 12, 6, 6, 200, 200, 200);
            byte[] t6 = new byte[36];
            for (int i = 0; i < t6.length; i++) t6[i] = (byte) 200;
            Matcher.Frame f = Matcher.prepare(rgba, 32, 32, 0);
            Matcher.Hit d1 = Matcher.findImage(f, t6, 6, 6, 0, 0, 0, 32, 32);
            boolean q1 = d1 != null && d1.x == 10 && d1.y == 12;
            rgbaSet(rgba, 32, 12, 14, 199, 199, 199);
            f = Matcher.prepare(rgba, 32, 32, 0);
            Matcher.Hit d2 = Matcher.findImage(f, t6, 6, 6, 0, 0, 0, 32, 32);
            boolean q2 = d2 == null;
            Matcher.Hit d3 = Matcher.findImage(f, t6, 6, 6, 8, 0, 0, 32, 32);
            boolean q3 = d3 != null && d3.x == 10 && d3.y == 12;

            byte[] b1x = rgbaSolid(120, 160, 30, 30, 30);
            rgbaFillRect(b1x, 120, 40, 60, 16, 16, 220, 220, 220);
            byte[] t16 = new byte[256];
            for (int i = 0; i < t16.length; i++) t16[i] = (byte) 220;
            Matcher.Frame fb = Matcher.prepare(b1x, 120, 160, 0);
            Matcher.Hit p1 = Matcher.findImage(fb, t16, 16, 16, 0, 0, 0, 120, 160);
            boolean q4 = p1 != null && p1.x == 40 && p1.y == 60;
            int[] ref = directRefExact(fb, t16, 16, 16);
            boolean q5 = ref != null && p1 != null && ref[0] == p1.x && ref[1] == p1.y;
            byte[] b2x = rgbaSolid(120, 160, 30, 30, 30);
            rgbaFillRect(b2x, 120, 21, 31, 16, 16, 220, 220, 220);
            Matcher.Frame fb2 = Matcher.prepare(b2x, 120, 160, 0);
            Matcher.Hit p2 = Matcher.findImage(fb2, t16, 16, 16, 0, 0, 0, 120, 160);
            boolean q6 = p2 != null && p2.x == 21 && p2.y == 31;

            byte[] rgba1 = rgbaSolid(16, 16, 10, 10, 10);
            rgbaSet(rgba1, 16, 7, 7, 100, 100, 100);
            Matcher.Frame f1 = Matcher.prepare(rgba1, 16, 16, 0);
            byte[] tm8 = new byte[]{(byte) 108};
            boolean q7 = Matcher.findImage(f1, tm8, 1, 1, 8, 0, 0, 16, 16) != null;
            byte[] tm9 = new byte[]{(byte) 109};
            boolean q8 = Matcher.findImage(f1, tm9, 1, 1, 8, 0, 0, 16, 16) == null;

            rec(out, logFile, (q1 && q2 && q3 && q4 && q5 && q6 && q7 && q8 ? "[OK] " : "[FAIL] ")
                    + "匹配 找图：直搜=" + q1 + " 扰动后thresh0不中=" + q2 + " thresh8命中=" + q3
                    + " 金字塔=" + q4 + " 与直搜一致=" + q5 + " 对齐偏移=" + q6
                    + " SAD==limit命中=" + q7 + " 超限不中=" + q8);
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配找图异常: " + e); }

        // 6) 颜色校验（48 容差含等号 / 无存色跳过）
        try {
            byte[] rgba = rgbaSolid(16, 16, 100, 120, 140);
            Matcher.Frame f = Matcher.prepare(rgba, 16, 16, 0);
            boolean cc1 = Matcher.colorCheck(f, 2, 3, 4, 4, (100 << 16) | (120 << 8) | 140);
            boolean cc2 = Matcher.colorCheck(f, 2, 3, 4, 4, (52 << 16) | (120 << 8) | 140);
            boolean cc3 = !Matcher.colorCheck(f, 2, 3, 4, 4, (51 << 16) | (120 << 8) | 140);
            boolean cc4 = Matcher.colorCheck(f, 2, 3, 4, 4, -1);
            rec(out, logFile, (cc1 && cc2 && cc3 && cc4 ? "[OK] " : "[FAIL] ")
                    + "匹配 颜色校验：同色=" + cc1 + " Δ48=" + cc2 + " Δ49拒=" + cc3 + " 无存色跳过=" + cc4);
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配颜色校验异常: " + e); }

        // 7) 性能冒烟：1440×3168 噪点帧 + 64×64 模板
        try {
            int W = 1440, H = 3168;
            byte[] big = new byte[W * H * 4];
            for (int i = 0; i < W * H; i++) {
                int v = (int) ((i * 2654435761L) >>> 16) & 0xFF;
                big[i * 4] = (byte) v; big[i * 4 + 1] = (byte) v; big[i * 4 + 2] = (byte) v;
                big[i * 4 + 3] = (byte) 255;
            }
            long t0 = System.currentTimeMillis();
            Matcher.Frame f = Matcher.prepare(big, W, H, 0);
            byte[] t64 = new byte[64 * 64];
            for (int yy = 0; yy < 64; yy++)
                for (int xx = 0; xx < 64; xx++)
                    t64[yy * 64 + xx] = f.gray[(1500 + yy) * W + 1000 + xx];
            long t1 = System.currentTimeMillis();
            Matcher.Hit h = Matcher.findImage(f, t64, 64, 64, 0, 0, 0, W, H);
            long t2 = System.currentTimeMillis();
            boolean ok = h != null && h.x == 1000 && h.y == 1500;
            rec(out, logFile, (ok && (t2 - t0) < 5000 ? "[OK] " : "[FAIL] ")
                    + "匹配 性能冒烟：命中=" + ok + " prepare=" + (t1 - t0) + "ms match=" + (t2 - t1) + "ms");
        } catch (Throwable e) { rec(out, logFile, "[FAIL] 匹配性能冒烟异常: " + e); }
    }

    /** 测试用：RGBA 纯色帧。 */
    private static byte[] rgbaSolid(int w, int h, int r, int g, int b) {
        byte[] a = new byte[w * h * 4];
        for (int i = 0; i < w * h; i++) {
            a[i * 4] = (byte) r; a[i * 4 + 1] = (byte) g; a[i * 4 + 2] = (byte) b; a[i * 4 + 3] = (byte) 255;
        }
        return a;
    }

    /** 测试用：改一个像素。 */
    private static void rgbaSet(byte[] a, int w, int x, int y, int r, int g, int b) {
        int p = (y * w + x) * 4;
        a[p] = (byte) r; a[p + 1] = (byte) g; a[p + 2] = (byte) b; a[p + 3] = (byte) 255;
    }

    /** 测试用：矩形填色。 */
    private static void rgbaFillRect(byte[] a, int w, int x, int y, int bw, int bh, int r, int g, int b) {
        for (int yy = 0; yy < bh; yy++)
            for (int xx = 0; xx < bw; xx++) rgbaSet(a, w, x + xx, y + yy, r, g, b);
    }

    /** 测试用直搜参考（全分辨率、精确比对；SAD=0 场景下应与金字塔同命中位）。 */
    private static int[] directRefExact(Matcher.Frame f, byte[] tmpl, int tw, int th) {
        int xe = f.w - tw, ye = f.h - th;
        for (int y = 0; y <= ye; y++)
            for (int x = 0; x <= xe; x++) {
                boolean ok = true;
                for (int yy = 0; yy < th && ok; yy++)
                    for (int xx = 0; xx < tw; xx++)
                        if ((f.gray[(y + yy) * f.w + x + xx] & 0xFF) != (tmpl[yy * tw + xx] & 0xFF)) { ok = false; break; }
                if (ok) return new int[]{x, y};
            }
        return null;
    }



    // ---------------------------------------------------------------- 渲染 + 布局断言

    private static void renderPages(final Context ctx, final List<String> out, final File logFile,
                                    final Progress cb, final int startIdx, final int total) {
        final Shell[] sh = new Shell[1];
        Throwable e = mainSync(new Runnable() {
            public void run() {
                Shell s = new Shell();
                s.build(ctx);
                sh[0] = s;
            }
        });
        if (e != null || sh[0] == null) {
            rec(out, logFile, "[FAIL] 构建界面树异常: " + e);
            return;
        }
        String[] expect = {"画布", "新建操作", "新建空方案", "模板", "核心控制"};   /* v13.1：区域页入口已合并（打开画布 / 空态＋新建…在画布上拖画 都含「画布」） */
        String[] pname = {"区域", "操作", "方案", "素材", "状态"};
        for (int i = 0; i < 5; i++) {
            if (cb != null) cb.step("渲染 " + pname[i] + "页", startIdx + i, total);
            final int idx = i;
            Throwable es = mainSync(new Runnable() {
                public void run() { sh[0].showTab(idx); }
            });
            if (es != null) rec(out, logFile, "[WARN] p" + idx + " 切页异常: " + es);
            try { Thread.sleep(2600); } catch (InterruptedException ig) { }
            rec(out, logFile, "[INFO] p" + idx + " 渲染开始");
            final List<String> viol = new ArrayList<String>();
            final boolean[] found = new boolean[1];
            Throwable e2 = mainSync(new Runnable() {
                public void run() {
                    renderView(ctx, sh[0].root, "render_p" + idx + ".png");
                    dlgDiag(sh[0].root, ctx, "diag_p" + idx + ".txt");
                    assertLayout(sh[0].root, "p" + idx, viol, ctx);
                    found[0] = containsText(sh[0].root, expect[idx]);
                }
            });
            if (e2 != null) {
                rec(out, logFile, "[FAIL] p" + idx + " 渲染异常: " + e2);
                continue;
            }
            StringBuilder sb = new StringBuilder();
            sb.append(found[0] ? "[OK] " : "[FAIL] ")
              .append("p").append(idx).append(' ').append(pname[idx]).append("页：渲染")
              .append(found[0] ? "（关键文本命中）" : "（缺关键文本「" + expect[idx] + "」，疑似渲染错页）");
            if (viol.isEmpty()) sb.append(" · 布局断言全过");
            else sb.append(" · 布局提示 ").append(viol.size()).append(" 条");
            rec(out, logFile, sb.toString());
            for (String v : viol) rec(out, logFile, "[WARN] " + v);
        }
    }

    /** 扩展检查：图标自检（PackageManager 资源解析）+ 对话框样式预览 + 符号探针。 */
    private static void extraChecks(final Context ctx, final List<String> out, final File logFile,
                                    final Progress cb, final int idx, final int total) {
        if (cb != null) cb.step("图标/编辑预览", idx, total);
        // 1) 图标自检（走资源解析，能真正验证 ARSC 是否可用）
        try {
            android.content.pm.ApplicationInfo ai = ctx.getPackageManager()
                    .getApplicationInfo(ctx.getPackageName(), 0);
            android.graphics.drawable.Drawable d = ai.loadIcon(ctx.getPackageManager());
            int w = d.getIntrinsicWidth(), h = d.getIntrinsicHeight();
            renderDrawable(ctx, d, "icon_check.png");
            rec(out, logFile, "[OK] 图标自检：resource=0x" + Integer.toHexString(ai.icon)
                    + "（期望 7f010000），载入 " + w + "x" + h);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 图标自检: " + t);
        }
        // 2) 对话框样式预览（内容区离屏渲染；window 边框为系统深色）
        Throwable e = mainSync(new Runnable() {
            public void run() {
                Ui.Dlg d = Ui.dialog(ctx, "删除区域 r1？");
                d.body.addView(Ui.dlgMessage(ctx, "引用它的操作绑定 / 门控会变为悬空（核心按未命中处理）。"));
                d.actions.addView(Ui.btnFlat(ctx, "取消", Ui.DIM));
                d.actions.addView(Ui.btnSmallTonal(ctx, "删除", Ui.WARN));
                renderWrap(ctx, d.root, "dlg_confirm.png", 0xFF0C0E10);
                /* 跨方案复制 冲突弹窗预览（v14；三按钮版式） */
                CrossCopy.Inspect ci = new CrossCopy.Inspect();
                ci.isRegion = false;
                ci.name = "挂机";
                ci.conflict = true;
                ci.renamePreview = "挂机-2";
                ci.carries.add("区域 r3");
                renderWrap(ctx, CrossCopy.buildConflictDialog(ctx, ci, "方案B", null, null).root,
                        "cross_dlg.png", 0xFF0C0E10);
                Ui.Dlg f = Ui.dialog(ctx, "编辑区域 r1");
                f.body.addView(Ui.label(ctx, "id（1..15 字，可用中文）"));
                android.widget.EditText et = Ui.textField(ctx, "如 r1");
                et.setText("r1");
                f.body.addView(et);
                f.body.addView(Ui.label(ctx, "类型"));
                f.body.addView(Ui.dlgMessage(ctx, "（预览：表单样式）"));
                f.actions.addView(Ui.btnFlat(ctx, "取消", Ui.DIM));
                f.actions.addView(Ui.btnSmallPrimary(ctx, "保存"));
                renderWrap(ctx, f.root, "dlg_form.png", 0xFF0C0E10);
                // 3) 编辑页预览（重构后的全屏编辑表单，离屏渲染）
                Regions.Region sr = new Regions.Region("r1");
                sr.a1 = 2325; sr.a2 = 686; sr.a3 = 2456; sr.a4 = 784;
                java.util.List<String> opsList = new java.util.ArrayList<String>();
                opsList.add("op1");
                opsList.add("op2");
                RegionEdit re = RegionEdit.build(ctx, sr, opsList,
                        "屏帧 1440×3168 · 坐标 0..1439 / 0..3167", null);
                renderWrap(ctx, re.root, "edit_region.png", Ui.BG);
                Ops.Step sv = new Ops.Step();
                sv.type = Ops.T_TAP;
                sv.a1 = 720; sv.a2 = 1584; sv.ms = 50;
                java.util.List<String> idsList = new java.util.ArrayList<String>();
                idsList.add("c1");
                idsList.add("s1");
                StepEdit se = StepEdit.build(ctx, sv, idsList, idsList, null, null, new String[Ops.VAR_N], null);
                renderWrap(ctx, se.root, "edit_step.png", Ui.BG);
                // 3.0b) 计算步 / 找图步编辑页预览（v6 新编辑器）
                Ops.Step sc = new Ops.Step();
                sc.type = Ops.T_CALC; sc.a1 = 0; sc.ref = "dx"; sc.expr = "atan2(tuy-tdy, tux-tdx)";
                StepEdit se2 = StepEdit.build(ctx, sc, idsList, idsList, null, null, new String[Ops.VAR_N], null);
                renderWrap(ctx, se2.root, "edit_calc.png", Ui.BG);
                Ops.Step sf = new Ops.Step();
                sf.type = Ops.T_FINDIMAGE; sf.a1 = 8; sf.ref = "tpl1"; sf.expr = "c1"; sf.a4 = Ops.C_CONT;
                StepEdit.Visual dv = new StepEdit.Visual() {
                    public void newTmpl(Forms.Chips field) { }
                    public void editTmpl(String name) { }
                    public void testTmpl(String name, int thresh, boolean cc, String region) { }
                    public void newPts(Forms.Chips field) { }
                    public void editPts(String name) { }
                    public void testPts(String name, String region) { }
                    public void pickPointXY(Forms.Num x, Forms.Num y) { }
                    public void pickColor(StepEdit.ColorSink sink) { }
                };
                StepEdit se3 = StepEdit.build(ctx, sf, idsList, idsList, null, null, new String[Ops.VAR_N], dv);
                renderWrap(ctx, se3.root, "edit_find.png", Ui.BG);
                boolean entryOk = containsText(se3.root, "新建模板");
                rec(out, logFile, (entryOk ? "[OK] " : "[FAIL] ") + "步骤素材入口：找图行含 [＋ 新建模板]=" + entryOk);
                // 3.1) 字段渲染探针（排查数字框不显示文本）
                android.widget.LinearLayout fp = Ui.col(ctx);
                fp.setPadding(Ui.dp(ctx, 10), Ui.dp(ctx, 10), Ui.dp(ctx, 10), Ui.dp(ctx, 10));
                android.widget.EditText fA = new android.widget.EditText(ctx);
                fA.setText("111"); fA.setTextColor(Ui.TEXT);
                fp.addView(fA);
                android.widget.EditText fB = Ui.numField(ctx, "hintB");
                fB.setText("222");
                fp.addView(fB);
                android.widget.EditText fC = Ui.numField(ctx, "hintC");
                fp.addView(fC);
                android.widget.EditText fD = new android.widget.EditText(ctx);
                fD.setHint("0");
                fD.setInputType(android.text.InputType.TYPE_CLASS_NUMBER
                        | android.text.InputType.TYPE_NUMBER_FLAG_SIGNED);
                fD.setBackground(Ui.round(ctx, Ui.CARD2, 10));
                fD.setPadding(Ui.dp(ctx, 10), Ui.dp(ctx, 8), Ui.dp(ctx, 10), Ui.dp(ctx, 8));
                fD.setText("444"); fD.setTextColor(Ui.TEXT);
                fp.addView(fD);
                android.widget.EditText fE = new android.widget.EditText(ctx);
                fE.setSingleLine(true);
                fE.setInputType(android.text.InputType.TYPE_CLASS_NUMBER
                        | android.text.InputType.TYPE_NUMBER_FLAG_SIGNED);
                fE.setGravity(android.view.Gravity.CENTER_HORIZONTAL);
                fE.setText("555"); fE.setTextColor(Ui.TEXT);
                fp.addView(fE);
                renderWrap(ctx, fp, "field_probe.png", Ui.BG);
                // 3.2) 移动步骤对话框内容预览（真文案）
                java.util.List<String> mvItems = new java.util.ArrayList<String>();
                mvItems.add("移到第 1 位（开头）");
                mvItems.add("移到第 2 位");
                mvItems.add("移到第 3 位");
                mvItems.add("移到第 5 位（末尾）");
                Ui.Dlg md = Forms.pickDialogBuild(ctx, "移动「4. 点击」到…", mvItems, -1, null);
                renderWrap(ctx, md.root, "move_dialog.png", 0xFF0C0E10);
                // 3.3) 步骤流程总览预览（时间线，含条件分支与跳转）
                java.util.List<Ops.Step> fsteps = new java.util.ArrayList<Ops.Step>();
                Ops.Step f1 = new Ops.Step(); f1.type = Ops.T_TAP; f1.a1 = 720; f1.a2 = 1584; f1.ms = 50;
                fsteps.add(f1);
                Ops.Step f2 = new Ops.Step(); f2.type = Ops.T_SWIPE;
                f2.a1 = 300; f2.a2 = 2000; f2.a3 = 900; f2.a4 = 1600; f2.ms = 180;
                fsteps.add(f2);
                Ops.Step f3 = new Ops.Step(); f3.type = Ops.T_WAIT; f3.ms = 120;
                fsteps.add(f3);
                Ops.Step f4 = new Ops.Step(); f4.type = Ops.T_COND_REGION; f4.ref = "c1";
                f4.a1 = 720; f4.a2 = 1584; f4.a4 = Ops.C_CONT; f4.a3 = Ops.C_JUMP; f4.j2 = 1;
                fsteps.add(f4);
                Ops.Step f5 = new Ops.Step(); f5.type = Ops.T_JUMP; f5.a1 = 0;
                fsteps.add(f5);
                android.view.View sfv = StepFlow.build(ctx, fsteps, null);
                renderWrap(ctx, sfv, "step_flow.png", Ui.BG);
                // 4) 符号探针（供按钮图标选型）
                android.widget.LinearLayout g = Ui.col(ctx);
                g.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 12), Ui.dp(ctx, 12), Ui.dp(ctx, 12));
                g.addView(Ui.tv(ctx, "✎ ⟳ ✕ ★ ✓ ↻ ⚙ ⌫ ✚ ＋ ↑ ↓ ◉ ▸", 22, Ui.TEXT, false));
                g.addView(Ui.tv(ctx, "✏ 🗑 ⛔ ⇄ ⊞ ⋮ ≡ − × ⟲ ⎋ › ⚠ ●", 22, Ui.TEXT, false));
                renderWrap(ctx, g, "glyphs.png", Ui.BG);
                // 4.1) 等宽探针（数字/ID/中文混排；v5 mono 口径验证）
                android.widget.LinearLayout mp = Ui.col(ctx);
                mp.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 12), Ui.dp(ctx, 12), Ui.dp(ctx, 12));
                mp.addView(Ui.mono(ctx, "0123456789 abcXYZ · r1 s2 c1 · pid 6035 · 3168×1440", 14, Ui.TEXT));
                mp.addView(Ui.mono(ctx, "640,1080 → 1024,1860 · 共 3 个区域 · op1 · 完整按压", 14, Ui.DIM));
                mp.addView(Ui.tv(ctx, "对照（非等宽）：0123456789 abcXYZ · 共 3 个区域", 14, Ui.DIM, false));
                renderWrap(ctx, mp, "mono_probe.png", Ui.BG);
                // 4.2) 光晕探针（运行态胶囊光晕 + 当前卡内高光；软渲染验证）
                android.widget.LinearLayout gp = Ui.col(ctx);
                gp.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 16), Ui.dp(ctx, 16), Ui.dp(ctx, 16));
                android.widget.LinearLayout glowRow = Ui.row(ctx);
                android.widget.TextView g1 = Ui.chip(ctx, "● 核心未运行", Ui.DIM);
                g1.setTypeface(android.graphics.Typeface.MONOSPACE);
                glowRow.addView(g1);
                android.widget.TextView g2 = Ui.tv(ctx, "● 核心运行中 pid 6035", 11, Ui.OK, false);
                g2.setTypeface(android.graphics.Typeface.MONOSPACE);
                g2.setIncludeFontPadding(false);
                g2.setPadding(Ui.dp(ctx, 8), Ui.dp(ctx, 4), Ui.dp(ctx, 8), Ui.dp(ctx, 4));
                android.widget.LinearLayout.LayoutParams g2lp = new android.widget.LinearLayout.LayoutParams(
                        android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
                g2lp.leftMargin = Ui.dp(ctx, 12);
                g2.setLayoutParams(g2lp);
                g2.setBackground(Ui.glowChip(ctx, Ui.OK));
                glowRow.addView(g2);
                gp.addView(glowRow);
                android.widget.LinearLayout gc = Ui.col(ctx);
                gc.setBackground(Ui.cardGlow(ctx));
                gc.setPadding(Ui.dp(ctx, 12), Ui.dp(ctx, 12), Ui.dp(ctx, 12), Ui.dp(ctx, 12));
                android.widget.LinearLayout.LayoutParams gclp = new android.widget.LinearLayout.LayoutParams(
                        android.widget.LinearLayout.LayoutParams.MATCH_PARENT, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
                gclp.topMargin = Ui.dp(ctx, 16);
                gc.setLayoutParams(gclp);
                gc.addView(Ui.monoBold(ctx, "ganjiangmoye", 15, Ui.ACCENT));
                gc.addView(Ui.mono(ctx, "区域 3 · 操作 1", 11, Ui.DIM));
                gp.addView(gc);
                renderWrap(ctx, gp, "glow_probe.png", Ui.BG);
                // 4.3) 品牌探针（记号 + 字标 + 记号尺寸阶梯）
                android.widget.LinearLayout bp = Ui.col(ctx);
                bp.setPadding(Ui.dp(ctx, 16), Ui.dp(ctx, 14), Ui.dp(ctx, 16), Ui.dp(ctx, 14));
                android.widget.LinearLayout brow = Ui.row(ctx);
                View bm = new View(ctx);
                bm.setBackground(Ui.brandMark(ctx));
                android.widget.LinearLayout.LayoutParams bmlp = new android.widget.LinearLayout.LayoutParams(Ui.dp(ctx, 20), Ui.dp(ctx, 20));
                bmlp.rightMargin = Ui.dp(ctx, 9);
                bm.setLayoutParams(bmlp);
                brow.addView(bm);
                android.widget.TextView bw = Ui.tv(ctx, "vtouch", 16, Ui.TEXT, false);
                bw.setTypeface(Ui.brandFont(ctx));
                brow.addView(bw);
                bp.addView(brow);
                android.widget.LinearLayout bmr = Ui.row(ctx);
                bmr.setPadding(0, Ui.dp(ctx, 12), 0, 0);
                int[] sizes = new int[]{28, 20, 14};
                for (int bi = 0; bi < sizes.length; bi++) {
                    View mk = new View(ctx);
                    mk.setBackground(Ui.brandMark(ctx));
                    android.widget.LinearLayout.LayoutParams mlp2 = new android.widget.LinearLayout.LayoutParams(Ui.dp(ctx, sizes[bi]), Ui.dp(ctx, sizes[bi]));
                    mlp2.rightMargin = Ui.dp(ctx, 10);
                    mk.setLayoutParams(mlp2);
                    bmr.addView(mk);
                }
                bp.addView(bmr);
                renderWrap(ctx, bp, "brand_probe.png", Ui.BG);
            }
        });
        if (e != null) {
            rec(out, logFile, "[FAIL] 对话框/编辑页预览异常: " + e);
        } else {
            rec(out, logFile, "[OK] 对话框/编辑页预览：dlg_confirm / dlg_form / cross_dlg / edit_region / edit_step / move_dialog / step_flow；符号探针 glyphs.png + mono 探针 mono_probe.png + 光晕探针 glow_probe.png + 品牌探针 brand_probe.png + 步骤预览 edit_calc / edit_find");
        }
    }

    /** 把 Drawable 画成 PNG（图标自检用）。 */
    private static void renderDrawable(Context ctx, android.graphics.drawable.Drawable d, String name) {
        try {
            int w = Math.max(1, d.getIntrinsicWidth() > 0 ? d.getIntrinsicWidth() : 256);
            int h = Math.max(1, d.getIntrinsicHeight() > 0 ? d.getIntrinsicHeight() : 256);
            Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            d.setBounds(0, 0, w, h);
            d.draw(c);
            FileOutputStream fo = new FileOutputStream(new File(ctx.getExternalFilesDir(null), name));
            bmp.compress(Bitmap.CompressFormat.PNG, 90, fo);
            fo.close();
            bmp.recycle();
        } catch (Throwable t) {
            Log.i(TAG, "renderDrawable " + name + " 异常: " + t);
        }
    }

    /** 按内容尺寸渲染（对话框预览/符号探针用）：AT_MOST 宽度 + 指定底色。 */
    private static void renderWrap(Context ctx, View v, String name, int bgColor) {
        try {
            int maxW = (int) (ctx.getResources().getDisplayMetrics().widthPixels * 0.86);
            v.measure(View.MeasureSpec.makeMeasureSpec(maxW, View.MeasureSpec.AT_MOST),
                      View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            int w = v.getMeasuredWidth() + Ui.dp(ctx, 16);
            int h = v.getMeasuredHeight() + Ui.dp(ctx, 16);
            v.layout(Ui.dp(ctx, 8), Ui.dp(ctx, 8), Ui.dp(ctx, 8) + v.getMeasuredWidth(),
                    Ui.dp(ctx, 8) + v.getMeasuredHeight());
            Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            c.drawColor(bgColor);
            v.draw(c);
            FileOutputStream fo = new FileOutputStream(new File(ctx.getExternalFilesDir(null), name));
            bmp.compress(Bitmap.CompressFormat.PNG, 90, fo);
            fo.close();
            bmp.recycle();
        } catch (Throwable t) {
            Log.i(TAG, "renderWrap " + name + " 异常: " + t);
        }
    }

    /** 把视图树画成 PNG（未挂载窗口：强制测量+布局后再绘制；息屏可用）。 */
    private static void renderView(Context ctx, View v, String name) {
        try {
            int w = ctx.getResources().getDisplayMetrics().widthPixels;
            int h = ctx.getResources().getDisplayMetrics().heightPixels - Ui.dp(ctx, 40);
            v.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                      View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
            v.layout(0, 0, w, h);
            Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            v.draw(c);
            FileOutputStream fo = new FileOutputStream(new File(ctx.getExternalFilesDir(null), name));
            bmp.compress(Bitmap.CompressFormat.PNG, 90, fo);
            fo.close();
            bmp.recycle();
        } catch (Throwable t) {
            Log.i(TAG, "renderView " + name + " 异常: " + t);
        }
    }

    /** v13.3：指定宽高渲染（横屏压力档）。 */
    private static void renderViewWh(Context ctx, View v, String name, int w, int h) {
        try {
            v.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
            v.layout(0, 0, w, h);
            Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            v.draw(c);
            FileOutputStream fo = new FileOutputStream(new File(ctx.getExternalFilesDir(null), name));
            bmp.compress(Bitmap.CompressFormat.PNG, 90, fo);
            fo.close();
            bmp.recycle();
        } catch (Throwable t) {
            Log.i(TAG, "renderViewWh " + name + " 异常: " + t);
        }
    }

    /** v13.3：只查溢出（横向/纵向越界）——画布页紧凑键允许 <40dp，热区断言不适用；防「按钮被裁看不见」。 */
    private static void assertOverflow(View v, String tag, List<String> viol, Context ctx) {
        if (v.getVisibility() != View.VISIBLE) return;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            int pw = g.getWidth(), ph = g.getHeight();
            boolean scroll = g instanceof ScrollView;
            for (int i = 0; i < g.getChildCount(); i++) {
                View c = g.getChildAt(i);
                if (c.getVisibility() == View.GONE) continue;
                if (c.getRight() > pw + 4) viol.add(tag + " 横向溢出 " + describe(c) + " right=" + c.getRight() + " > " + pw);
                if (!scroll && c.getBottom() > ph + 4) viol.add(tag + " 纵向溢出 " + describe(c) + " bottom=" + c.getBottom() + " > " + ph);
            }
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) assertOverflow(g.getChildAt(i), tag, viol, ctx);
        }
    }

    /** 布局断言：横向/纵向越界、点击热区 <40dp、文本非预期截断（仅可见视图）。 */
    private static void assertLayout(View v, String page, List<String> viol, Context ctx) {
        if (v.getVisibility() != View.VISIBLE) return;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            int pw = g.getWidth(), ph = g.getHeight();
            boolean scroll = g instanceof ScrollView;
            for (int i = 0; i < g.getChildCount(); i++) {
                View c = g.getChildAt(i);
                if (c.getVisibility() == View.GONE) continue;
                if (c.getRight() > pw + 4)
                    viol.add(page + " 横向溢出 " + describe(c) + " right=" + c.getRight() + " > 容器 " + pw);
                if (!scroll && c.getBottom() > ph + 4)
                    viol.add(page + " 纵向溢出 " + describe(c) + " bottom=" + c.getBottom() + " > 容器 " + ph);
            }
        }
        if (v.isClickable() && v.getWidth() < Ui.dp(ctx, 40) || v.isClickable() && v.getHeight() < Ui.dp(ctx, 40)) {
            viol.add(page + " 热区偏小 " + describe(v) + " " + v.getWidth() + "x" + v.getHeight());
        }
        if (v instanceof TextView) {
            TextView t = (TextView) v;
            if (t.getEllipsize() == null && t.getLayout() != null) {
                for (int l = 0; l < t.getLayout().getLineCount(); l++) {
                    if (t.getLayout().getEllipsisCount(l) > 0) {
                        viol.add(page + " 文本意外截断 " + describe(t));
                        break;
                    }
                }
            }
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) assertLayout(g.getChildAt(i), page, viol, ctx);
        }
    }

    private static String describe(View v) {
        if (v instanceof TextView) return v.getClass().getSimpleName() + "[" + ((TextView) v).getText() + "]";
        return v.getClass().getSimpleName();
    }

    private static boolean containsText(View v, String needle) {
        if (v instanceof TextView) {
            CharSequence t = ((TextView) v).getText();
            if (t != null && t.toString().contains(needle)) return true;
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++)
                if (containsText(g.getChildAt(i), needle)) return true;
        }
        return false;
    }

    /** 布局诊断：文本控件实际边界 → diag_p<i>.txt。 */
    private static void dlgDiag(View v, Context ctx, String name) {
        try {
            StringBuilder sb = new StringBuilder();
            dumpView(v, sb, 0);
            FileOutputStream fo = new FileOutputStream(new File(ctx.getExternalFilesDir(null), name));
            fo.write(sb.toString().getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ig) { }
    }

    private static void dumpView(View v, StringBuilder sb, int d) {
        if (v instanceof TextView) {
            TextView t = (TextView) v;
            for (int i = 0; i < d; i++) sb.append(' ');
            sb.append(t.getClass().getSimpleName()).append(" [").append(t.getText()).append("] @")
              .append(v.getLeft()).append(',').append(v.getTop()).append(' ')
              .append(v.getWidth()).append('x').append(v.getHeight()).append('\n');
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) dumpView(g.getChildAt(i), sb, d + 1);
        }
    }

    // ---------------------------------------------------------------- 工具

    /** 把 runnable 投到主线程执行并等待；返回异常（含超时），null = 成功。 */
    private static Throwable mainSync(final Runnable r) {
        final CountDownLatch l = new CountDownLatch(1);
        final Throwable[] err = new Throwable[1];
        Ui.MAIN.post(new Runnable() {
            public void run() {
                try { r.run(); }
                catch (Throwable t) { err[0] = t; }
                finally { l.countDown(); }
            }
        });
        try {
            if (!l.await(25, TimeUnit.SECONDS)) return new RuntimeException("mainSync 超时（25s）");
        } catch (InterruptedException e) {
            return e;
        }
        return err[0];
    }

    private static void rec(List<String> out, File logFile, String line) {
        out.add(line);
        try {
            FileOutputStream fo = new FileOutputStream(logFile, true);
            fo.write((ts() + " " + line + "\n").getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ig) { }
        Log.i(TAG, line);
    }

    // ---------------------------------------------------------------- 比较器（原 MainActivity 移入）

    public static boolean regionsEqual(Regions.File a, Regions.File b) {
        if (a.regions.size() != b.regions.size()) return false;
        if (!eq(a.frameLine, b.frameLine)) return false;
        if (!a.hideIds.equals(b.hideIds)) return false;
        if (!a.unknown.equals(b.unknown)) return false;
        for (int i = 0; i < a.regions.size(); i++) {
            Regions.Region x = a.regions.get(i), y = b.regions.get(i);
            if (!x.id.equals(y.id) || x.type != y.type || x.a1 != y.a1 || x.a2 != y.a2
                    || x.a3 != y.a3 || x.a4 != y.a4 || x.en != y.en || x.kind != y.kind
                    || !x.bindOp.equals(y.bindOp) || !x.bindEv.equals(y.bindEv)) return false;
        }
        return true;
    }

    public static boolean opsEqual(Ops.File a, Ops.File b) {
        if (a.ops.size() != b.ops.size() || a.version != b.version) return false;
        if (!a.unknown.equals(b.unknown)) return false;
        for (int i = 0; i < a.ops.size(); i++) {
            Ops.Op x = a.ops.get(i), y = b.ops.get(i);
            if (!x.name.equals(y.name) || !x.gate.equals(y.gate) || x.autooff != y.autooff) return false;
            if (!eqExtra(x.extra, y.extra)) return false;
            if (x.steps.size() != y.steps.size()) return false;
            for (int k = 0; k < x.steps.size(); k++) {
                Ops.Step s = x.steps.get(k), t = y.steps.get(k);
                if (s.type != t.type || s.a1 != t.a1 || s.a2 != t.a2 || s.a3 != t.a3 || s.a4 != t.a4
                        || s.ms != t.ms || s.j1 != t.j1 || s.j2 != t.j2
                        || !s.ref.equals(t.ref) || !s.expr.equals(t.expr) || !eq(s.raw, t.raw)) return false;
            }
        }
        return true;
    }

    private static boolean eq(String x, String y) {
        return x == null ? y == null : x.equals(y);
    }

    /** 第 10 字段空哨兵：v4+ 序列化统一补写 "-"（9 字段行 → 10 字段归一化），语义上 null/空/"-" 等价。 */
    private static boolean eqExtra(String x, String y) {
        String a = (x == null || x.isEmpty()) ? "-" : x;
        String b = (y == null || y.isEmpty()) ? "-" : y;
        return a.equals(b);
    }
    // ---------------------------------------------------------------- 抓帧 / 帧库（v7 T3）

    /** 抓帧/帧库检查：真机抓帧（尺寸/非黑/耗时）+ scratch 帧库 12存10轮换与往返。 */
    private static void frameChecks(Context ctx, List<String> out, File logFile) {
        try {
            Capture.init(ctx);
            FrameStore.init(ctx);
            long t0 = System.currentTimeMillis();
            Capture.Shot s = Capture.grab();
            long dt = System.currentTimeMillis() - t0;
            boolean landscape = (s.rot == 1 || s.rot == 3);
            boolean sz = landscape ? (s.w == 3168 && s.h == 1440) : (s.w == 1440 && s.h == 3168);
            boolean len = s.rgba.length == s.w * s.h * 4;
            long sum = 0;
            int n = 0;
            for (int i = 0; i + 2 < s.rgba.length; i += 4099 * 4) {
                sum += (s.rgba[i] & 0xFF) + (s.rgba[i + 1] & 0xFF) + (s.rgba[i + 2] & 0xFF);
                n++;
            }
            boolean lit = n > 0 && (sum / (3.0 * n)) > 2;
            rec(out, logFile, (sz && len && lit ? "[OK] " : "[FAIL] ") + "抓帧：尺寸=" + s.w + "x" + s.h
                    + " rot=" + s.rot + " 非黑=" + lit + " 耗时=" + dt + "ms");
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 抓帧异常: " + e);
        }
        File scratch = new File(ctx.getExternalFilesDir(null), "frames-test");
        try {
            scratch.mkdirs();
            FrameStore.setDir(scratch);
            for (int i = 0; i < 12; i++) {
                Capture.Shot s = new Capture.Shot();
                s.w = 8; s.h = 8; s.rot = 0;
                s.rgba = new byte[8 * 8 * 4];
                for (int k = 0; k < 64; k++) {
                    s.rgba[k * 4] = (byte) (10 + i);
                    s.rgba[k * 4 + 1] = (byte) 20;
                    s.rgba[k * 4 + 2] = (byte) 30;
                    s.rgba[k * 4 + 3] = (byte) 255;
                }
                Thread.sleep(2);
                FrameStore.save(s);
            }
            List<FrameStore.Entry> ls = FrameStore.list();
            boolean cnt = ls.size() == 10;
            FrameStore.Entry lt = FrameStore.latest();
            byte[] rb = lt != null ? FrameStore.loadRgba(lt) : null;
            boolean rbOk = rb != null && rb.length == 8 * 8 * 4
                    && (rb[0] & 0xFF) == 21 && (rb[1] & 0xFF) == 20 && (rb[2] & 0xFF) == 30;
            android.graphics.Bitmap bm = lt != null ? FrameStore.loadBitmap(lt) : null;
            boolean bmOk = bm != null && bm.getWidth() == 8 && bm.getHeight() == 8;
            rec(out, logFile, (cnt && lt != null && rbOk && bmOk ? "[OK] " : "[FAIL] ") + "帧库：轮换10=" + cnt
                    + " 最新读取=" + rbOk + " 位图=" + bmOk + "（共 " + ls.size() + "）");
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 帧库异常: " + e);
        } finally {
            FrameStore.setDir(null);
            deleteRec(scratch);
        }
    }

    private static void deleteRec(File f) {
        File[] ch = f.listFiles();
        if (ch != null) for (File c : ch) deleteRec(c);
        f.delete();
    }


    // ---------------------------------------------------------------- 看图器（v7 T4）

    /** 看图器检查：screen↔frame 往返 / 缩放钳制 / 框最小与钳制 / 离屏渲染件。 */
    private static void imageChecks(Context ctx, final List<String> out, final File logFile) {
        try {
            final android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(400, 600, android.graphics.Bitmap.Config.ARGB_8888);
            final ImageView[] box = new ImageView[1];
            Throwable err = mainSync(new Runnable() {
                public void run() {
                    ImageView v = new ImageView(ctx);
                    v.setFrame(bm, 0);
                    int W = Ui.dp(ctx, 360);
                    int H = Ui.dp(ctx, 480);
                    v.measure(android.view.View.MeasureSpec.makeMeasureSpec(W, android.view.View.MeasureSpec.EXACTLY),
                            android.view.View.MeasureSpec.makeMeasureSpec(H, android.view.View.MeasureSpec.EXACTLY));
                    v.layout(0, 0, W, H);
                    box[0] = v;
                }
            });
            if (err != null || box[0] == null) throw new Exception("构建失败: " + err);
            ImageView v = box[0];
            boolean rt = true;
            float[] zooms = {1f, 2.5f, 8f};
            for (float z : zooms) {
                v.setZoom(z, v.getWidth() / 2f, v.getHeight() / 2f);
                int[][] pts = {{0, 0}, {399, 599}, {123, 456}};
                for (int[] p : pts) {
                    int[] sp = v.frameToScreen(p[0], p[1]);
                    int[] fp = v.screenToFrame(sp[0], sp[1]);
                    if (Math.abs(fp[0] - p[0]) > 1 || Math.abs(fp[1] - p[1]) > 1) rt = false;
                }
            }
            v.setZoom(100f, 0, 0);
            boolean z1 = v.zoom() <= 8.0001f;
            v.setZoom(0.01f, 0, 0);
            boolean z2 = v.zoom() >= 0.999f;
            v.setSelection(10, 10, 5, 5);
            int[] s1 = v.selection();
            boolean b1 = s1[2] >= 8 && s1[3] >= 8;
            v.setSelection(390, 590, 50, 50);
            int[] s2 = v.selection();
            boolean b2 = s2[0] >= 0 && s2[1] >= 0 && s2[0] + s2[2] <= 400 && s2[1] + s2[3] <= 600;
            rec(out, logFile, (rt && z1 && z2 && b1 && b2 ? "[OK] " : "[FAIL] ") + "看图器 数学：往返=" + rt
                    + " 缩放钳制=" + (z1 && z2) + " 框最小=" + b1 + " 框钳制=" + b2);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 看图器数学异常: " + e);
        }
        try {
            Throwable err = mainSync(new Runnable() {
                public void run() {
                    ImageView v = new ImageView(ctx);
                    android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(400, 600, android.graphics.Bitmap.Config.ARGB_8888);
                    android.graphics.Canvas c = new android.graphics.Canvas(bm);
                    android.graphics.Paint p = new android.graphics.Paint();
                    for (int y = 0; y < 600; y += 20) {
                        p.setColor(0xFF203040);
                        c.drawRect(0, y, 400, y + 10, p);
                    }
                    p.setColor(0xFFE8B14F);
                    c.drawCircle(200, 300, 60, p);
                    v.setFrame(bm, 0);
                    v.setMode(ImageView.MODE_BOX);
                    v.setSelection(150, 220, 100, 120);
                    v.setHit(160, 300, 64, 40);
                    renderWrap(ctx, v, "frame_view.png", Ui.BG);
                    int[] s = v.selection();
                    if (!(s[0] == 150 && s[1] == 220 && s[2] == 100 && s[3] == 120))
                        throw new RuntimeException("selection 回读不符: " + s[0] + "," + s[1] + "," + s[2] + "," + s[3]);
                }
            });
            rec(out, logFile, (err == null ? "[OK] " : "[FAIL] ") + "看图器 渲染件 frame_view.png"
                    + (err == null ? "（selection 回读一致）" : " 异常: " + err));
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 看图器渲染异常: " + e);
        }
    }


    // ---------------------------------------------------------------- 会话页（v7 T6）

    /** 会话页检查：外壳叠层 / 模板保存链(scratch) / 渲染件 edit_tmpl.png。 */
    private static void viewChecks(Context ctx, List<String> out, File logFile) {
        // 1) 外壳叠层：open/close/返回键优先
        try {
            final boolean[] lr = new boolean[1];
            Throwable e = mainSync(new Runnable() {
                public void run() {
                    Shell sh = new Shell();
                    sh.build(ctx);
                    android.view.View v = new android.view.View(ctx);
                    sh.openLayer(v, null);
                    boolean o1 = sh.layerOpen();
                    sh.closeLayer();
                    boolean o2 = !sh.layerOpen();
                    final Shell shh = sh;
                    final boolean[] bk = new boolean[1];
                    sh.openLayer(v, new Runnable() {
                        public void run() { bk[0] = true; shh.closeLayer(); }
                    });
                    boolean b1 = sh.closeEditor();
                    boolean b2 = bk[0] && !sh.layerOpen();
                    lr[0] = o1 && o2 && b1 && b2;
                }
            });
            rec(out, logFile, (e == null && lr[0] ? "[OK] " : "[FAIL] ") + "外壳叠层：open/close/返回优先=" + lr[0]
                    + (e != null ? " 异常: " + e : ""));
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 外壳叠层异常: " + e);
        }
        // 2) 模板保存链（scratch 帧库 + scratch 素材目录）+ 渲染件
        File fsScratch = new File(ctx.getExternalFilesDir(null), "frames-test2");
        String oldADir = Assets.dir;
        try {
            fsScratch.mkdirs();
            FrameStore.setDir(fsScratch);
            Assets.dir = fsScratch.getAbsolutePath() + "/templates";
            final int FW = 480, FH = 900;
            byte[] rgba = new byte[FW * FH * 4];
            for (int i = 0; i < FW * FH; i++) {
                int v = (int) ((i * 2654435761L) >>> 16) & 0xFF;
                rgba[i * 4] = (byte) v;
                rgba[i * 4 + 1] = (byte) v;
                rgba[i * 4 + 2] = (byte) v;
                rgba[i * 4 + 3] = (byte) 255;
            }
            Capture.Shot shot = new Capture.Shot();
            shot.w = FW;
            shot.h = FH;
            shot.rot = 0;
            shot.rgba = rgba;
            FrameStore.save(shot);
            final VisualPage[] vp = new VisualPage[1];
            final CountDownLatch latch = new CountDownLatch(1);
            Throwable err = mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_TMPL_NEW;
                    VisualPage p = new VisualPage(ctx, ses, null);
                    p.attach(new Runnable() {
                        public void run() { latch.countDown(); }
                    });
                    vp[0] = p;
                }
            });
            boolean ready = err == null && latch.await(10, TimeUnit.SECONDS);
            final String[] res = new String[1];
            if (ready) {
                Throwable e2 = mainSync(new Runnable() {
                    public void run() {
                        vp[0].setSelectionForTest(100, 200, 64, 64);
                        vp[0].setNameForTest("t_e2e");
                        res[0] = vp[0].saveAndTest();
                    }
                });
                if (e2 != null) res[0] = "异常: " + e2;
            }
            Assets.Tmpl tm = Assets.readTmpl("t_e2e");
            int[] avg = new int[3];
            byte[] want = Assets.grayFromRgba(rgba, FW, 100, 200, 64, 64, avg);
            boolean fileOk = tm != null && tm.w == 64 && tm.h == 64 && tm.rot == 0 && tm.x == 100 && tm.y == 200
                    && tm.avgRgb == ((avg[0] << 16) | (avg[1] << 8) | avg[2]) && eqArr(tm.gray, want);
            boolean hitOk = res[0] != null && res[0].contains("命中");
            rec(out, logFile, (ready && fileOk && hitOk ? "[OK] " : "[FAIL] ") + "会话 模板保存链(scratch)：ready="
                    + ready + " 文件一致=" + fileOk + " 试一下=" + (res[0] == null ? "null" : res[0]));
            if (ready) {
                Throwable e3 = mainSync(new Runnable() {
                    public void run() { renderView(ctx, vp[0].root, "edit_tmpl.png"); }
                });
                boolean hasBtn = e3 == null && containsText(vp[0].root, "保存");
                rec(out, logFile, (hasBtn ? "[OK] " : "[FAIL] ") + "会话 渲染件 edit_tmpl.png（含保存按钮="
                        + hasBtn + (e3 != null ? " 异常: " + e3 : "") + "）");
            }
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 会话保存链异常: " + e);
        } finally {
            FrameStore.setDir(null);
            Assets.dir = oldADir;
            deleteRec(fsScratch);
        }
    }


    // ---------------------------------------------------------------- 会话页 T7（取点/吸色/点集）

    /** 会话页 T7 检查：取点（预览→确认）/ 吸色 / 点集保存链(scratch) / 模糊提示。 */
    private static void viewPickChecks(Context ctx, List<String> out, File logFile) {
        File scratch = new File(ctx.getExternalFilesDir(null), "frames-test3");
        String oldADir = Assets.dir;
        final int FW = 480, FH = 900;
        try {
            scratch.mkdirs();
            FrameStore.setDir(scratch);
            Assets.dir = scratch.getAbsolutePath() + "/templates";
            // 帧1：噪点 + 特征像素 (200,60,70) @(10,20)
            byte[] rgba = new byte[FW * FH * 4];
            for (int i = 0; i < FW * FH; i++) {
                int v = (int) ((i * 2654435761L) >>> 16) & 0xFF;
                rgba[i * 4] = (byte) v;
                rgba[i * 4 + 1] = (byte) v;
                rgba[i * 4 + 2] = (byte) v;
                rgba[i * 4 + 3] = (byte) 255;
            }
            int p0 = (20 * FW + 10) * 4;
            rgba[p0] = (byte) 200;
            rgba[p0 + 1] = (byte) 60;
            rgba[p0 + 2] = (byte) 70;
            Capture.Shot shot = new Capture.Shot();
            shot.w = FW;
            shot.h = FH;
            shot.rot = 0;
            shot.rgba = rgba;
            FrameStore.save(shot);
            // ---- 取点：点选预览、[确认] 提交 ----
            final int[] commits = new int[1];
            final int[] commitArg = new int[2];
            final VisualPage[] pp = new VisualPage[1];
            final CountDownLatch l1 = new CountDownLatch(1);
            Throwable e1 = mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_PICK_POINT;
                    VisualPage p = new VisualPage(ctx, ses, new VisualPage.Done() {
                        public void onTmplSaved(String name) { }
                        public void onPointPicked(int lx, int ly) { commits[0]++; commitArg[0] = lx; commitArg[1] = ly; }
                        public void onColorPicked(int rgb) { commits[0]++; }
                        public void onCanceled() { }
                    });
                    p.attach(new Runnable() {
                        public void run() { l1.countDown(); }
                    });
                    pp[0] = p;
                }
            });
            boolean r1 = e1 == null && l1.await(10, TimeUnit.SECONDS);
            mainSync(new Runnable() {
                public void run() { pp[0].onIvPoint(100, 120); }
            });
            boolean previewOk = r1 && commits[0] == 0 && pp[0].resultText().contains("逻辑 (100,120)");
            mainSync(new Runnable() {
                public void run() { pp[0].pickConfirm(); }
            });
            boolean commitOk = commits[0] == 1 && commitArg[0] == 100 && commitArg[1] == 120;
            rec(out, logFile, (previewOk && commitOk ? "[OK] " : "[FAIL] ") + "会话 取点：预览=" + previewOk
                    + " 确认提交=" + commitOk + (r1 ? "" : " 载帧失败"));
            // ---- 吸色 ----
            final int[] rgbGot = new int[1];
            final VisualPage[] pc = new VisualPage[1];
            final CountDownLatch l2 = new CountDownLatch(1);
            mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_PICK_COLOR;
                    VisualPage p = new VisualPage(ctx, ses, new VisualPage.Done() {
                        public void onTmplSaved(String name) { }
                        public void onPointPicked(int lx, int ly) { }
                        public void onColorPicked(int rgb) { rgbGot[0] = rgb; }
                        public void onCanceled() { }
                    });
                    p.attach(new Runnable() {
                        public void run() { l2.countDown(); }
                    });
                    pc[0] = p;
                }
            });
            boolean r2 = l2.await(10, TimeUnit.SECONDS);
            mainSync(new Runnable() {
                public void run() { pc[0].onIvPoint(10, 20); }
            });
            boolean colorOk = r2 && pc[0].lastPickRgb == 0xC83C46;
            mainSync(new Runnable() {
                public void run() { pc[0].pickConfirm(); }
            });
            boolean colorCommit = rgbGot[0] == 0xC83C46;
            rec(out, logFile, (colorOk && colorCommit ? "[OK] " : "[FAIL] ") + "会话 吸色：取样=" + colorOk
                    + " 确认提交=" + colorCommit);
            // ---- 点集保存链：bg10 + 基色(200,0,0)@(50,60) + 参考点(0,200,0)@(70,60) ----
            byte[] rgba2 = new byte[FW * FH * 4];
            for (int i = 0; i < FW * FH; i++) {
                rgba2[i * 4] = 10;
                rgba2[i * 4 + 1] = 10;
                rgba2[i * 4 + 2] = 10;
                rgba2[i * 4 + 3] = (byte) 255;
            }
            int q0 = (60 * FW + 50) * 4;
            rgba2[q0] = (byte) 200;
            rgba2[q0 + 1] = 0;
            rgba2[q0 + 2] = 0;
            int q1 = (60 * FW + 70) * 4;
            rgba2[q1] = 0;
            rgba2[q1 + 1] = (byte) 200;
            rgba2[q1 + 2] = 0;
            Capture.Shot shot2 = new Capture.Shot();
            shot2.w = FW;
            shot2.h = FH;
            shot2.rot = 0;
            shot2.rgba = rgba2;
            FrameStore.save(shot2);
            final VisualPage[] ps = new VisualPage[1];
            final CountDownLatch l3 = new CountDownLatch(1);
            mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_PTS_NEW;
                    VisualPage p = new VisualPage(ctx, ses, null);
                    p.attach(new Runnable() {
                        public void run() { l3.countDown(); }
                    });
                    ps[0] = p;
                }
            });
            boolean r3 = l3.await(10, TimeUnit.SECONDS);
            mainSync(new Runnable() {
                public void run() {
                    ps[0].ptsTestTap(50, 60);
                    ps[0].ptsTestTap(70, 60);
                }
            });
            final String[] ptsRes = new String[1];
            mainSync(new Runnable() {
                public void run() { ptsRes[0] = ps[0].ptsSaveAndTest("p_e2e"); }
            });
            Assets.Pts ptsFile = Assets.readPts("p_e2e");
            boolean ptsFileOk = ptsFile != null && ptsFile.baseRgb == 0xC80000 && ptsFile.pts.length == 1
                    && ptsFile.pts[0].dx == 20 && ptsFile.pts[0].dy == 0 && ptsFile.pts[0].rgb == 0x00C800
                    && ptsFile.hasXy && ptsFile.x == 50 && ptsFile.y == 60;
            boolean ptsHitOk = ptsRes[0] != null && ptsRes[0].contains("命中");
            boolean ptsCountOk = ps[0].ptsCount() == 1;
            rec(out, logFile, (r3 && ptsFileOk && ptsHitOk && ptsCountOk ? "[OK] " : "[FAIL] ")
                    + "会话 点集保存链(scratch)：ready=" + r3 + " 文件=" + ptsFileOk
                    + " 试一下=" + (ptsRes[0] == null ? "null" : ptsRes[0]) + " 点数=" + ps[0].ptsCount());
            {
                boolean n1 = Assets.nameOk("微信头像") && Assets.nameOk("第1关") && Regions.idOk("按钮_上")
                        && Regions.idOk("一二三四五六七八九十百千万亿零")
                        && !Assets.nameOk("一二三四五六七八九十百千万亿零甲")
                        && !Assets.nameOk("按钮 A") && !Assets.nameOk("-") && !Assets.nameOk("a/b")
                        && !Assets.nameOk("名 ") && !Assets.nameOk("名​称")
                        && Assets.nameOk("😀😀😀😀😀😀😀😀😀😀😀")
                        && !Assets.nameOk("😀😀😀😀😀😀😀😀😀😀😀😀")
                        && Regions.varOk("角度") && !Regions.varOk("1角度") && !Regions.varOk("角度-1");
                rec(out, logFile, (n1 ? "[OK] " : "[FAIL] ") + "命名 v11（中文/边界/UTF-8/字节上限）");
            }
            {
                /* v12 [框区域]：命名/换算/序列化（纯函数，离线核） */
                Regions.File rf = Regions.File.parse("#vtouch-regions v2\n#frame 0 1440 3168\nregion r1 0 10 20 30 40 1\n");
                boolean nOk = "r2".equals(RegionDraw.nextId(rf, false)) && "c1".equals(RegionDraw.nextId(rf, true));
                int[] lr = RegionDraw.rectToLogic(1, 100, 200, 10, 20, 30, 40);   /* rot1 手算: (179,10)(139,40) → 归一 [139,10,179,40] */
                boolean rOk = lr[0] == 139 && lr[1] == 10 && lr[2] == 179 && lr[3] == 40;
                int[] lc = RegionDraw.circleToLogic(0, 200, 200, 100, 50, 30);
                boolean cOk = lc[0] == 100 && lc[1] == 50 && lc[2] == 30;
                Regions.Region cir = RegionDraw.make("c1", true, 100, 50, 30, 0);
                rf.regions.add(cir);
                Regions.File rt = Regions.File.parse(rf.serialize());
                Regions.Region back = rt.find("c1");
                boolean sOk = back != null && back.type == 1 && back.a1 == 100 && back.a2 == 50 && back.a3 == 30
                        && back.en == 1 && "-".equals(back.bindOp) && back.kind == 0 && "r1".equals(rt.regions.get(0).id);
                rec(out, logFile, (nOk && rOk && cOk && sOk ? "[OK] " : "[FAIL] ")
                        + "区域框选 v12（命名/换算/序列化）n=" + nOk + " rect=" + rOk + " circ=" + cOk + " ser=" + sOk);
            }
            {
                /* v12b 区域帧上编辑：换算往返（rot0 精确 / rot1 ±1px / 圆） */
                int[] f0 = Matcher.logicRectToFrame(0, 320, 640, 100, 200, 200, 200);
                int[] b0 = RegionDraw.rectToLogic(0, 320, 640, f0[0], f0[1], f0[2], f0[3]);
                boolean rt0 = b0[0] == 100 && b0[1] == 200 && b0[2] == 300 && b0[3] == 400;
                int[] f1 = Matcher.logicRectToFrame(1, 320, 640, 100, 200, 200, 200);
                int[] b1 = RegionDraw.rectToLogic(1, 320, 640, f1[0], f1[1], f1[2], f1[3]);
                boolean rt1 = Math.abs(b1[0] - 100) <= 1 && Math.abs(b1[1] - 200) <= 1
                        && Math.abs(b1[2] - 300) <= 1 && Math.abs(b1[3] - 400) <= 1;
                int[] cfx = RegionDraw.circleToFrame(1, 320, 640, 100, 200, 50);
                int[] cbk = RegionDraw.circleToLogic(1, 320, 640, cfx[0], cfx[1], cfx[2]);
                boolean rtc = Math.abs(cbk[0] - 100) <= 1 && Math.abs(cbk[1] - 200) <= 1 && Math.abs(cbk[2] - 50) <= 1;
                rec(out, logFile, (rt0 && rt1 && rtc ? "[OK] " : "[FAIL] ")
                        + "区域帧上编辑 v12b（换算往返）rot0=" + rt0 + " rot1=" + rt1 + " 圆=" + rtc);
            }
            {
                /* v12b 会话级：预载表单几何 → 拖改 → [使用此框] 回填（矩形 + 圆，rot=1 帧） */
                byte[] rgbaR = new byte[FW * FH * 4];
                for (int i = 0; i < FW * FH; i++) { rgbaR[i * 4 + 3] = (byte) 255; }
                Capture.Shot shotR = new Capture.Shot();
                shotR.w = FW; shotR.h = FH; shotR.rot = 1; shotR.rgba = rgbaR;
                FrameStore.save(shotR);
                final int[][] gotR = new int[1][], gotC = new int[1][], selAt = new int[1][], cirAt = new int[1][];
                final boolean[] cirR = new boolean[1], cirC = new boolean[1];
                final int[] cancelR = new int[2];
                final VisualPage[] pvR = new VisualPage[1], pvC = new VisualPage[1];
                final CountDownLatch l5 = new CountDownLatch(1), l6 = new CountDownLatch(1);
                mainSync(new Runnable() {
                    public void run() {
                        VisualPage.Session ses = new VisualPage.Session();
                        ses.mode = VisualPage.MODE_REGION;
                        ses.regionCircle = false;
                        ses.regionGeom = new int[]{100, 200, 300, 400};
                        VisualPage p = new VisualPage(ctx, ses, new VisualPage.Done() {
                            public void onTmplSaved(String n) { }
                            public void onPointPicked(int x, int y) { }
                            public void onColorPicked(int rgb) { }
                            public void onCanceled() { cancelR[0]++; }
                            public void onRegionPicked(int[] geom, boolean circle) { gotR[0] = geom; cirR[0] = circle; }
                        });
                        p.attach(new Runnable() { public void run() { l5.countDown(); } });
                        pvR[0] = p;
                    }
                });
                boolean r5 = l5.await(10, TimeUnit.SECONDS);
                mainSync(new Runnable() {
                    public void run() { selAt[0] = pvR[0].imageView().selection(); }
                });
                boolean preloadOk = r5 && selAt[0] != null && selAt[0][0] == 100 && selAt[0][1] == 200
                        && selAt[0][2] == 200 && selAt[0][3] == 200;   /* v12e：基帧0 = 原位，不随帧 rot 旋转 */
                mainSync(new Runnable() {
                    public void run() { pvR[0].setSelectionForTest(210, 610, 200, 200); }
                });
                mainSync(new Runnable() {
                    public void run() { pvR[0].regionConfirm(); }
                });
                boolean rectOk = gotR[0] != null && gotR[0][0] == 210 && gotR[0][1] == 610
                        && gotR[0][2] == 410 && gotR[0][3] == 810 && !cirR[0] && cancelR[0] == 0;
                rec(out, logFile, (preloadOk && rectOk ? "[OK] " : "[FAIL] ")
                        + "会话 区域框选(矩形)：预载=" + preloadOk + " 回填=" + rectOk
                        + (rectOk ? "" : " got=" + java.util.Arrays.toString(gotR[0])) + (r5 ? "" : " 载帧失败"));
                mainSync(new Runnable() {
                    public void run() {
                        VisualPage.Session ses = new VisualPage.Session();
                        ses.mode = VisualPage.MODE_REGION;
                        ses.regionCircle = true;
                        ses.regionGeom = new int[]{300, 400, 100};   /* v12e：帧内值（帧 480×900，setCircle 会钳入帧） */
                        VisualPage p = new VisualPage(ctx, ses, new VisualPage.Done() {
                            public void onTmplSaved(String n) { }
                            public void onPointPicked(int x, int y) { }
                            public void onColorPicked(int rgb) { }
                            public void onCanceled() { cancelR[1]++; }
                            public void onRegionPicked(int[] geom, boolean circle) { gotC[0] = geom; cirC[0] = circle; }
                        });
                        p.attach(new Runnable() { public void run() { l6.countDown(); } });
                        pvC[0] = p;
                    }
                });
                boolean r6 = l6.await(10, TimeUnit.SECONDS);
                mainSync(new Runnable() {
                    public void run() { cirAt[0] = pvC[0].imageView().circle(); }
                });
                boolean circPre = r6 && cirAt[0] != null && cirAt[0][0] == 300 && cirAt[0][1] == 400 && cirAt[0][2] == 100;
                mainSync(new Runnable() {
                    public void run() { pvC[0].regionConfirm(); }
                });
                boolean circOk = circPre && gotC[0] != null && gotC[0][0] == 300 && gotC[0][1] == 400
                        && gotC[0][2] == 100 && cirC[0] && cancelR[1] == 0;
                rec(out, logFile, (circOk ? "[OK] " : "[FAIL] ")
                        + "会话 区域框选(圆)：预载=" + circPre + " 回填=" + circOk
                        + (circOk ? "" : " got=" + java.util.Arrays.toString(gotC[0])));
            }
            {
                /* v12c 全部区域总览：载入投影 → 编辑 → [保存全部] 回写（scratch 重定向） */
                String oldRO = RegionDraw.readOverride, oldPO = RegionDraw.pathOverride;
                boolean loadOk = false, saveOk = false, newOk = false;
                String saveMsg = "";
                final int[] cnt = new int[1];
                final VisualPage[] pvA = new VisualPage[1];
                try {
                    java.io.File rfile = new java.io.File(scratch, "regions-canvas.conf");
                    String init = "#vtouch-regions v2\n#frame 0 480 900\nregion r1 0 100 200 300 400 1\n"
                            + "bind r1 - down\nkind r1 0\nregion c1 1 380 620 80 0 1\nbind c1 - down\nkind c1 0\n";
                    java.io.FileOutputStream rf = new java.io.FileOutputStream(rfile);
                    rf.write(init.getBytes("UTF-8"));
                    rf.close();
                    RegionDraw.readOverride = rfile.getAbsolutePath();
                    RegionDraw.pathOverride = rfile.getAbsolutePath();
                    byte[] rgbaA = new byte[FW * FH * 4];
                    for (int i = 0; i < FW * FH; i++) { rgbaA[i * 4 + 3] = (byte) 255; }
                    Capture.Shot shotA = new Capture.Shot();
                    shotA.w = FW; shotA.h = FH; shotA.rot = 1; shotA.rgba = rgbaA;
                    FrameStore.save(shotA);
                    final CountDownLatch l7 = new CountDownLatch(1);
                    mainSync(new Runnable() {
                        public void run() {
                            VisualPage.Session ses = new VisualPage.Session();
                            ses.mode = VisualPage.MODE_REGIONS;
                            VisualPage p = new VisualPage(ctx, ses, null);
                            p.attach(new Runnable() { public void run() { l7.countDown(); } });
                            pvA[0] = p;
                        }
                    });
                    boolean r7 = l7.await(10, TimeUnit.SECONDS);
                    final int[][] g1 = new int[1][], g2 = new int[1][];
                    mainSync(new Runnable() {
                        public void run() {
                            java.util.List<ImageView.RegShape> shs = pvA[0].imageView().regions();
                            cnt[0] = shs.size();
                            if (shs.size() > 0) g1[0] = shs.get(0).g.clone();
                            if (shs.size() > 1) g2[0] = shs.get(1).g.clone();
                        }
                    });
                    loadOk = r7 && cnt[0] == 2
                            && g1[0][0] == 100 && g1[0][1] == 200 && g1[0][2] == 200 && g1[0][3] == 200
                            && g2[0][0] == 380 && g2[0][1] == 620 && g2[0][2] == 80;   /* v12e：基帧0 → 原位投影 */
                    /* 模拟拖改：r1 右移 100 帧像素 → 保存全部 → 校验盘上 */
                    mainSync(new Runnable() {
                        public void run() {
                            java.util.List<ImageView.RegShape> shs = pvA[0].imageView().regions();
                            shs.get(0).g[0] = 250;
                            shs.get(0).moved = true;      /* 等价真实拖动（MOVE 处理置位） */
                        }
                    });
                    /* v12d：画具拖画新建（rNew* 直吃帧坐标）→ 应生成并命名 r2、选中 */
                    mainSync(new Runnable() {
                        public void run() {
                            ImageView riv = pvA[0].imageView();
                            riv.setNewArm(1);
                            riv.rNewBegin(50, 60);
                            riv.rNewMove(150, 160);
                            riv.rNewFinish();
                        }
                    });
                    final int[] cntA = new int[1];
                    final String[] newId = new String[1];
                    final int[][] g2b = new int[1][];
                    mainSync(new Runnable() {
                        public void run() {
                            java.util.List<ImageView.RegShape> shs = pvA[0].imageView().regions();
                            cntA[0] = shs.size();
                            if (shs.size() > 2) { newId[0] = shs.get(2).id; g2b[0] = shs.get(2).g.clone(); }
                        }
                    });
                    newOk = cntA[0] == 3 && "r2".equals(newId[0]) && g2b[0] != null
                            && g2b[0][0] == 50 && g2b[0][1] == 60 && g2b[0][2] == 100 && g2b[0][3] == 100;
                    try { saveMsg = pvA[0].saveAllRegionsSync(); } catch (Throwable t) { saveMsg = "EX:" + t; }
                    byte[] bb = new byte[(int) rfile.length()];
                    java.io.FileInputStream fi = new java.io.FileInputStream(rfile);
                    fi.read(bb); fi.close();
                    String outTxt = new String(bb, "UTF-8");
                    saveOk = outTxt.contains("region r1 0 250 200 450 400 1")
                            && outTxt.contains("bind r1 - down")
                            && outTxt.contains("region c1 1 380 620 80 0 1")
                            && outTxt.contains("region r2 0 50 60 150 160 1")
                            && outTxt.contains("bind r2 - down");
                    mainSync(new Runnable() {
                        public void run() { pvA[0].selectRegionForTest(2); }
                    });
                    if (r7) {
                        Throwable e7 = mainSync(new Runnable() {
                            public void run() { renderView(ctx, pvA[0].root, "edit_regions.png"); }
                        });
                        rec(out, logFile, (e7 == null ? "[OK] " : "[FAIL] ")
                                + "会话 渲染件 edit_regions.png（全部区域）" + (e7 != null ? " 异常: " + e7 : ""));
                    }
                } catch (Throwable t7) {
                    saveMsg = saveMsg + " EX:" + t7;
                } finally {
                    RegionDraw.readOverride = oldRO;
                    RegionDraw.pathOverride = oldPO;
                }
                rec(out, logFile, (loadOk && saveOk && newOk ? "[OK] " : "[FAIL] ")
                        + "会话 全部区域调整：载入投影=" + loadOk + " 新建=" + newOk + " 保存回写=" + saveOk
                        + " cnt=" + cnt[0] + " msg=" + saveMsg);
            }
            {
                /* v12e 基帧口径（#frame）：区域数字 = 基帧视口坐标——横屏帧不再被帧方向二次旋转。
                 * 回归：基帧 0 + (2325,686) 在横屏帧上曾被算成 (686,-886)（飞出画面看不见）。 */
                Regions.File bf0 = Regions.File.parse("#vtouch-regions v2\n#frame 0 1440 3168\nregion r1 0 2325 686 2456 784 1\n");
                boolean bfOk = bf0.frameRot == 0 && bf0.frameW == 1440 && bf0.frameH == 3168;
                int[] gA = Matcher.logicRectToFrame(bf0.frameRot, bf0.frameW, bf0.frameH, 2325, 686, 131, 98);
                boolean gAok = gA[0] == 2325 && gA[1] == 686 && gA[2] == 131 && gA[3] == 98;
                Regions.File bf1 = Regions.File.parse("#vtouch-regions v2\n#frame 1 3168 1440\nregion x 0 655 2325 753 2456 1\n");
                int[] gB = Matcher.logicRectToFrame(bf1.frameRot, bf1.frameW, bf1.frameH, 655, 2325, 98, 131);
                boolean gBok = gB[0] == 2325 && Math.abs(gB[1] - 686) <= 1 && gB[2] == 131 && gB[3] == 98;
                int[] rB = RegionDraw.rectToLogic(bf1.frameRot, bf1.frameW, bf1.frameH, gB[0], gB[1], gB[2], gB[3]);
                boolean rBok = Math.abs(rB[0] - 655) <= 1 && rB[1] == 2325 && Math.abs(rB[2] - 753) <= 1 && rB[3] == 2456;
                rec(out, logFile, (bfOk && gAok && gBok && rBok ? "[OK] " : "[FAIL] ")
                        + "基帧投影 v12e（解析=" + bfOk + " 基帧0原位=" + gAok + " 基帧1视口=" + gBok + " 往返=" + rBok + "）");
            }
            if (r3) {
                Throwable e4 = mainSync(new Runnable() {
                    public void run() { renderView(ctx, ps[0].root, "edit_pts.png"); }
                });
                rec(out, logFile, (e4 == null ? "[OK] " : "[FAIL] ") + "会话 渲染件 edit_pts.png"
                        + (e4 != null ? " 异常: " + e4 : ""));
            }
            if (r2) {
                Throwable e5 = mainSync(new Runnable() {
                    public void run() { renderView(ctx, pc[0].root, "edit_pick.png"); }
                });
                rec(out, logFile, (e5 == null ? "[OK] " : "[FAIL] ") + "会话 渲染件 edit_pick.png"
                        + (e5 != null ? " 异常: " + e5 : ""));
            }
            // ---- 模糊提示：整帧同色 ----
            byte[] rgba3 = new byte[FW * FH * 4];
            for (int i = 0; i < FW * FH; i++) {
                rgba3[i * 4] = (byte) 200;
                rgba3[i * 4 + 1] = 0;
                rgba3[i * 4 + 2] = 0;
                rgba3[i * 4 + 3] = (byte) 255;
            }
            Capture.Shot shot3 = new Capture.Shot();
            shot3.w = FW;
            shot3.h = FH;
            shot3.rot = 0;
            shot3.rgba = rgba3;
            FrameStore.save(shot3);
            final VisualPage[] pb = new VisualPage[1];
            final CountDownLatch l4 = new CountDownLatch(1);
            mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_PTS_NEW;
                    VisualPage p = new VisualPage(ctx, ses, null);
                    p.attach(new Runnable() {
                        public void run() { l4.countDown(); }
                    });
                    pb[0] = p;
                }
            });
            boolean r4 = l4.await(10, TimeUnit.SECONDS);
            mainSync(new Runnable() {
                public void run() {
                    pb[0].ptsTestTap(10, 10);
                    pb[0].ptsTestTap(30, 10);
                }
            });
            mainSync(new Runnable() {
                public void run() { pb[0].ptsSaveAndTest("p_blur"); }
            });
            boolean blurOk = r4 && pb[0].lastBlurWarn();
            rec(out, logFile, (blurOk ? "[OK] " : "[FAIL] ") + "会话 点集模糊提示=" + blurOk);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] 会话 T7 检查异常: " + e);
        } finally {
            FrameStore.setDir(null);
            Assets.dir = oldADir;
            deleteRec(scratch);
        }
    }


    // ---------------------------------------------------------------- v7.1 修复轮

    /** v7.1 检查：拖画框选 / 底栏限高 / 帧库删除清空 / 内存直通 / 快速试查。 */
    private static void fixBatchChecks(Context ctx, List<String> out, File logFile) {
        // 1) 拖画框选（BOX 模式空选区拖动 = 画新框）
        try {
            final ImageView[] box = new ImageView[1];
            Throwable err = mainSync(new Runnable() {
                public void run() {
                    ImageView v = new ImageView(ctx);
                    v.setFrame(android.graphics.Bitmap.createBitmap(400, 600, android.graphics.Bitmap.Config.ARGB_8888), 0);
                    int W = Ui.dp(ctx, 360);
                    int H = Ui.dp(ctx, 480);
                    v.measure(android.view.View.MeasureSpec.makeMeasureSpec(W, android.view.View.MeasureSpec.EXACTLY),
                            android.view.View.MeasureSpec.makeMeasureSpec(H, android.view.View.MeasureSpec.EXACTLY));
                    v.layout(0, 0, W, H);
                    v.setMode(ImageView.MODE_BOX);
                    long t = android.os.SystemClock.uptimeMillis();
                    v.dispatchTouchEvent(android.view.MotionEvent.obtain(t, t, android.view.MotionEvent.ACTION_DOWN, 200, 300, 0));
                    v.dispatchTouchEvent(android.view.MotionEvent.obtain(t, t + 20, android.view.MotionEvent.ACTION_MOVE, 500, 700, 0));
                    v.dispatchTouchEvent(android.view.MotionEvent.obtain(t, t + 40, android.view.MotionEvent.ACTION_UP, 500, 700, 0));
                    box[0] = v;
                }
            });
            if (err != null || box[0] == null) throw new Exception("构建失败: " + err);
            int[] sel = box[0].selection();
            int[] e0 = box[0].screenToFrame(200, 300);
            int[] e1 = box[0].screenToFrame(500, 700);
            int ex = Math.min(e0[0], e1[0]);
            int ey = Math.min(e0[1], e1[1]);
            int ew = Math.abs(e1[0] - e0[0]);
            int eh = Math.abs(e1[1] - e0[1]);
            boolean ok = sel[2] > 8 && sel[3] > 8
                    && Math.abs(sel[0] - ex) <= 2 && Math.abs(sel[1] - ey) <= 2
                    && Math.abs(sel[2] - ew) <= 2 && Math.abs(sel[3] - eh) <= 2;
            rec(out, logFile, (ok ? "[OK] " : "[FAIL] ") + "v7.1 拖画框选：sel=" + sel[0] + "," + sel[1] + ","
                    + sel[2] + "," + sel[3] + " 期望=" + ex + "," + ey + "," + ew + "," + eh);
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] v7.1 拖画框选异常: " + e);
        }
        // 2) 底栏限高：16 参考点下画面仍 ≥35%
        File scratch = new File(ctx.getExternalFilesDir(null), "frames-fix1");
        String oldADir = Assets.dir;
        try {
            scratch.mkdirs();
            FrameStore.setDir(scratch);
            Assets.dir = scratch.getAbsolutePath() + "/templates";
            int FW = 400, FH = 700;
            byte[] rgba = rgbaSolid(FW, FH, 12, 12, 12);
            Capture.Shot shot = new Capture.Shot();
            shot.w = FW;
            shot.h = FH;
            shot.rot = 0;
            shot.rgba = rgba;
            FrameStore.save(shot);
            final VisualPage[] pv = new VisualPage[1];
            final CountDownLatch latch = new CountDownLatch(1);
            mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_PTS_NEW;
                    VisualPage p = new VisualPage(ctx, ses, null);
                    p.attach(new Runnable() {
                        public void run() { latch.countDown(); }
                    });
                    pv[0] = p;
                }
            });
            boolean ready = latch.await(10, TimeUnit.SECONDS);
            if (ready) {
                mainSync(new Runnable() {
                    public void run() {
                        for (int i = 0; i < 16; i++) pv[0].ptsTestTap(5 + i, 5 + i);
                    }
                });
            }
            final int[] hs = new int[2];
            mainSync(new Runnable() {
                public void run() {
                    int sw = ctx.getResources().getDisplayMetrics().widthPixels;
                    int sh = ctx.getResources().getDisplayMetrics().heightPixels;
                    pv[0].root.measure(android.view.View.MeasureSpec.makeMeasureSpec(sw, android.view.View.MeasureSpec.EXACTLY),
                            android.view.View.MeasureSpec.makeMeasureSpec(sh, android.view.View.MeasureSpec.EXACTLY));
                    pv[0].root.layout(0, 0, sw, sh);
                    hs[0] = pv[0].imageView().getHeight();
                    hs[1] = pv[0].root.getHeight();
                }
            });
            boolean ok = ready && hs[0] >= hs[1] * 0.35;
            rec(out, logFile, (ok ? "[OK] " : "[FAIL] ") + "v7.1 底栏限高：画面高 " + hs[0] + " / " + hs[1]
                    + "（≥35%=" + (hs[0] >= hs[1] * 0.35) + " 16 点）");
            // 3) 帧库删除 + 清空（先清空保证确定性：2 帧删 1 剩 1）
            FrameStore.clearAll();
            FrameStore.save(shot);
            FrameStore.save(shot);
            FrameStore.Entry cur = FrameStore.latest();
            boolean del1 = FrameStore.delete(cur) && FrameStore.list().size() == 1
                    && !new File(scratch, cur.name + ".png").exists();
            boolean del2 = FrameStore.clearAll() && FrameStore.list().isEmpty();
            rec(out, logFile, (del1 && del2 ? "[OK] " : "[FAIL] ") + "v7.1 帧库管理：删单帧=" + del1 + " 清空=" + del2);
            // 4) 内存直通：空库 + flash → 会话就绪且帧信息正确
            FrameStore.clearAll();
            byte[] rgba2 = rgbaSolid(320, 640, 30, 60, 90);
            Capture.Shot sh2 = new Capture.Shot();
            sh2.w = 320;
            sh2.h = 640;
            sh2.rot = 1;
            sh2.rgba = rgba2;
            FrameStore.flashShot = sh2;
            FrameStore.flashTs = System.currentTimeMillis();
            final VisualPage[] pv2 = new VisualPage[1];
            final CountDownLatch l2 = new CountDownLatch(1);
            mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_VIEW;
                    VisualPage p = new VisualPage(ctx, ses, null);
                    p.attach(new Runnable() {
                        public void run() { l2.countDown(); }
                    });
                    pv2[0] = p;
                }
            });
            boolean r2 = l2.await(10, TimeUnit.SECONDS);
            int[] fi = pv2[0].frameInfoForTest();
            boolean ok4 = r2 && fi[0] == 320 && fi[1] == 640 && fi[2] == 1 && FrameStore.flashShot == null;
            rec(out, logFile, (ok4 ? "[OK] " : "[FAIL] ") + "v7.1 内存直通：就绪=" + r2 + " 帧=" + fi[0] + "x" + fi[1]
                    + " rot=" + fi[2] + " 已消费=" + (FrameStore.flashShot == null));
            // 5) 快速试查：模板命中 / 旋转模板 / 未命中 / 点集
            byte[] rgbaQ = rgbaSolid(FW, FH, 12, 12, 12);   /* 独特色块帧：命中点唯一（纯色帧会处处 SAD=0） */
            for (int yy = 20; yy < 44; yy++) {
                for (int xx = 10; xx < 34; xx++) {
                    int o = (yy * FW + xx) * 4;
                    rgbaQ[o] = (byte) 220;   /* 220-12=208：单行错配 24×208=4992 > 阈值 4608 → 非精确位置必拒 */
                    rgbaQ[o + 1] = (byte) 220;
                    rgbaQ[o + 2] = (byte) 220;
                }
            }
            Assets.Tmpl tq = new Assets.Tmpl();
            tq.w = 24; tq.h = 24; tq.rot = 0; tq.x = 10; tq.y = 20; tq.hasXy = true; tq.avgRgb = -1;
            tq.gray = Assets.grayFromRgba(rgbaQ, FW, 10, 20, 24, 24, null);
            Assets.writeTmpl("qt_t", tq);
            QuickTest.Result q1 = QuickTest.run(rgbaQ, FW, FH, 0, "qt_t", false, 8);
            boolean q1ok = q1.hit && q1.x == 10 && q1.y == 20 && q1.w == 24 && q1.h == 24;
            Assets.Tmpl tr = new Assets.Tmpl();
            tr.w = 24; tr.h = 24; tr.rot = 1; tr.x = 0; tr.y = 0; tr.hasXy = false; tr.avgRgb = -1;
            tr.gray = new byte[24 * 24];
            for (int i = 0; i < tr.gray.length; i++) tr.gray[i] = (byte) (i % 251);
            Assets.writeTmpl("qt_r", tr);
            QuickTest.Result q2 = QuickTest.run(rgba, FW, FH, 0, "qt_r", false, 8);
            boolean q2ok = q2 != null && q2.text != null && !q2.text.contains("未实现") && !q2.text.contains("失败");
            Assets.Pts qp = new Assets.Pts();
            qp.baseRgb = 0x0C0C0C; qp.baseTol = 2; qp.x = 50; qp.y = 50; qp.hasXy = true;
            Assets.Pt qpt = new Assets.Pt();
            qpt.dx = 5; qpt.dy = 0; qpt.rgb = 0x0C0C0C; qpt.tol = 2;
            qp.pts = new Assets.Pt[]{qpt};
            Assets.writePts("qt_p", qp);
            QuickTest.Result q3 = QuickTest.run(rgba, FW, FH, 0, "qt_p", true, 8);
            boolean q3ok = q3.hit;
            QuickTest.Result q4 = QuickTest.run(rgba, FW, FH, 0, "no_such", false, 8);
            boolean q4ok = !q4.hit && q4.text != null && q4.text.contains("读不到");
            rec(out, logFile, (q1ok && q2ok && q3ok && q4ok ? "[OK] " : "[FAIL] ") + "v7.1 快速试查：模板=" + q1ok
                    + " 旋转=" + q2ok + " 点集=" + q3ok + " 缺素材=" + q4ok
                    + " | q1[hit=" + q1.hit + ",x=" + q1.x + ",y=" + q1.y + ",w=" + q1.w + ",h=" + q1.h
                    + ",t=" + q1.text + "]");
            // 6) 屏幕取点回填：闪存帧已消费的取色会话上 screenPicked(100,200) → 帧像素回填 + 实际取色
            FrameStore.clearAll();
            byte[] rp = rgbaSolid(400, 700, 50, 60, 70);
            int op = (200 * 400 + 100) * 4;
            rp[op] = (byte) 200;
            rp[op + 1] = (byte) 30;
            rp[op + 2] = (byte) 30;
            Capture.Shot sp = new Capture.Shot();
            sp.w = 400;
            sp.h = 700;
            sp.rot = 0;
            sp.rgba = rp;
            FrameStore.flashShot = sp;
            FrameStore.flashTs = System.currentTimeMillis();
            final VisualPage[] pvS = new VisualPage[1];
            final CountDownLatch lS = new CountDownLatch(1);
            mainSync(new Runnable() {
                public void run() {
                    VisualPage.Session ses = new VisualPage.Session();
                    ses.mode = VisualPage.MODE_PICK_COLOR;
                    VisualPage p = new VisualPage(ctx, ses, null);
                    p.attach(new Runnable() {
                        public void run() { lS.countDown(); }
                    });
                    pvS[0] = p;
                }
            });
            boolean rS = lS.await(10, TimeUnit.SECONDS);
            mainSync(new Runnable() {
                public void run() { pvS[0].screenPicked(100, 200); }
            });
            final int[] pk = new int[2];
            final String[] rs = new String[1];
            mainSync(new Runnable() {
                public void run() {
                    int[] q = pvS[0].pickedForTest();
                    pk[0] = q[0];
                    pk[1] = q[1];
                    rs[0] = pvS[0].resultText();
                }
            });
            boolean ok6 = rS && pk[0] == 100 && pk[1] == 200 && rs[0] != null && rs[0].contains("#C81E1E");
            rec(out, logFile, (ok6 ? "[OK] " : "[FAIL] ") + "v7.2 屏幕取点回填：帧像素=" + pk[0] + "," + pk[1]
                    + " 取色/结果=" + rs[0]);
            // 7) 脚本干跑：找图命中 / 点按跳过 / 找色单点命中 / 区域限定未命中 → DryRun 报告
            Ops.Op dop = new Ops.Op();
            dop.name = "dry1";
            Ops.Step dst1 = new Ops.Step();
            dst1.type = 10;
            dst1.a1 = 8;
            dst1.ref = "qt_t";
            dop.steps.add(dst1);
            Ops.Step dst2 = new Ops.Step();
            dst2.type = 1;
            dst2.a1 = 5;
            dst2.a2 = 6;
            dop.steps.add(dst2);
            Ops.Step dst3 = new Ops.Step();
            dst3.type = 11;
            dst3.a1 = 0;
            dst3.a2 = (0xDCDCDC << 8) | 4;
            dop.steps.add(dst3);
            Ops.Step dst4 = new Ops.Step();
            dst4.type = 10;
            dst4.a1 = 8;
            dst4.ref = "qt_t";
            dst4.expr = "r9";
            dop.steps.add(dst4);
            java.util.Map<String, int[]> regs = new java.util.HashMap<String, int[]>();
            regs.put("r9", new int[]{200, 300, 100, 100});
            DryRun.Report drep = DryRun.run(rgbaQ, FW, FH, 0, 0, FW, FH, dop, regs);
            boolean ok7 = drep.visual == 3 && drep.hits == 2 && drep.misses == 1 && drep.lines.size() == 3
                    && drep.lines.get(0).hit && drep.lines.get(0).x == 10 && drep.lines.get(0).y == 20
                    && drep.lines.get(1).hit && drep.lines.get(1).x == 10 && drep.lines.get(1).y == 20
                    && !drep.lines.get(2).hit;
            rec(out, logFile, (ok7 ? "[OK] " : "[FAIL] ") + "v8 脚本干跑：视觉步=" + drep.visual + " 命中=" + drep.hits
                    + " 未命中=" + drep.misses + " 行数=" + drep.lines.size()
                    + " | " + (drep.lines.size() > 0 ? drep.lines.get(0).text : ""));
            {
                /* v12e 横屏帧画布：横屏帧(900×450 rot1) + 基帧0 数据 → 区域按原位投影（回归硬门 + 渲染件）。 */
                String oldRO2 = RegionDraw.readOverride, oldPO2 = RegionDraw.pathOverride;
                boolean landOk = false;
                try {
                    java.io.File lfile = new java.io.File(scratch, "regions-land.conf");
                    String initL = "#vtouch-regions v2\n#frame 0 1440 3168\n"
                            + "region r1 0 600 100 700 150 1\nbind r1 - down\nkind r1 0\n"
                            + "region c1 1 300 180 60 0 1\nbind c1 - down\nkind c1 0\n";
                    java.io.FileOutputStream lf2 = new java.io.FileOutputStream(lfile);
                    lf2.write(initL.getBytes("UTF-8"));
                    lf2.close();
                    RegionDraw.readOverride = lfile.getAbsolutePath();
                    RegionDraw.pathOverride = lfile.getAbsolutePath();
                    byte[] rgbaL = new byte[900 * 450 * 4];
                    for (int i = 0; i < 900 * 450; i++) { rgbaL[i * 4 + 3] = (byte) 255; }
                    Capture.Shot shotL = new Capture.Shot();
                    shotL.w = 900; shotL.h = 450; shotL.rot = 1; shotL.rgba = rgbaL;
                    FrameStore.save(shotL);
                    final CountDownLatch l8 = new CountDownLatch(1);
                    final VisualPage[] pvL = new VisualPage[1];
                    mainSync(new Runnable() {
                        public void run() {
                            VisualPage.Session ses = new VisualPage.Session();
                            ses.mode = VisualPage.MODE_REGIONS;
                            VisualPage p = new VisualPage(ctx, ses, null);
                            p.attach(new Runnable() { public void run() { l8.countDown(); } });
                            pvL[0] = p;
                        }
                    });
                    boolean r8 = l8.await(10, TimeUnit.SECONDS);
                    final int[][] gl1 = new int[1][], gl2 = new int[1][];
                    mainSync(new Runnable() {
                        public void run() {
                            java.util.List<ImageView.RegShape> shs = pvL[0].imageView().regions();
                            if (shs.size() > 0) gl1[0] = shs.get(0).g.clone();
                            if (shs.size() > 1) gl2[0] = shs.get(1).g.clone();
                        }
                    });
                    landOk = r8 && gl1[0] != null && gl1[0][0] == 600 && gl1[0][1] == 100
                            && gl1[0][2] == 100 && gl1[0][3] == 50
                            && gl2[0] != null && gl2[0][0] == 300 && gl2[0][1] == 180 && gl2[0][2] == 60;
                    if (r8) {
                        Throwable e8 = mainSync(new Runnable() {
                            public void run() { renderView(ctx, pvL[0].root, "edit_regions_land.png"); }
                        });
                        if (e8 != null) landOk = false;
                    }
                } catch (Throwable t8) {
                    landOk = false;
                } finally {
                    RegionDraw.readOverride = oldRO2;
                    RegionDraw.pathOverride = oldPO2;
                }
                rec(out, logFile, (landOk ? "[OK] " : "[FAIL] ")
                        + "会话 横屏帧画布 v12e（基帧0 原位投影 + edit_regions_land.png）");
            }
            {
                /* v13 画布 2.0：方向视图/网格 + 数值编辑 + 属性（停用/绑定）+ 删除立即写盘（渲染件 edit_regions_grid.png）。 */
                String oldRO3 = RegionDraw.readOverride, oldPO3 = RegionDraw.pathOverride;
                boolean v13ok = false;
                String v13msg = "";
                try {
                    java.io.File vfile = new java.io.File(scratch, "regions-v13.conf");
                    String initV = "#vtouch-regions v2\n#frame 0 1440 3168\n"
                            + "region r1 0 100 200 300 400 1\nbind r1 - down\nkind r1 0\n"
                            + "region off1 0 2325 686 2456 784 1\nbind off1 - down\nkind off1 0\n";
                    java.io.FileOutputStream vf = new java.io.FileOutputStream(vfile);
                    vf.write(initV.getBytes("UTF-8"));
                    vf.close();
                    RegionDraw.readOverride = vfile.getAbsolutePath();
                    RegionDraw.pathOverride = vfile.getAbsolutePath();
                    byte[] rgbaV = new byte[900 * 450 * 4];
                    for (int i = 0; i < 900 * 450; i++) { rgbaV[i * 4 + 3] = (byte) 255; }
                    Capture.Shot shotV = new Capture.Shot();
                    shotV.w = 900; shotV.h = 450; shotV.rot = 1; shotV.rgba = rgbaV;
                    FrameStore.save(shotV);
                    final CountDownLatch l9 = new CountDownLatch(1);
                    final VisualPage[] pvV = new VisualPage[1];
                    mainSync(new Runnable() {
                        public void run() {
                            VisualPage.Session ses = new VisualPage.Session();
                            ses.mode = VisualPage.MODE_REGIONS;
                            VisualPage p = new VisualPage(ctx, ses, null);
                            p.attach(new Runnable() { public void run() { l9.countDown(); } });
                            pvV[0] = p;
                        }
                    });
                    boolean r9 = l9.await(10, TimeUnit.SECONDS);
                    /* ① 方向视图：切竖屏 → 网格 1440×3168；off1 完全屏外 → 计数 1；g 值不变（显示位置=运行时口径） */
                    final int[][] sp13 = new int[1][];
                    final int[][] gOff = new int[1][];
                    mainSync(new Runnable() {
                        public void run() {
                            pvV[0].setOrientForTest(0);
                            sp13[0] = pvV[0].canvasSpaceForTest();
                            java.util.List<ImageView.RegShape> shs = pvV[0].imageView().regions();
                            if (shs.size() > 1) gOff[0] = shs.get(1).g.clone();
                        }
                    });
                    boolean orientOk = sp13[0] != null && sp13[0][0] == 1440 && sp13[0][1] == 3168 && sp13[0][2] == 1 && sp13[0][3] == 1
                            && gOff[0] != null && gOff[0][0] == 2325 && gOff[0][1] == 686;
                    /* ② 数值编辑：选中 off1，向 X1 格注入 500 → 实例几何随动（等效检查器键入） */
                    final int[][] gNum = new int[1][];
                    mainSync(new Runnable() {
                        public void run() {
                            pvV[0].selectRegionForTest(1);
                            pvV[0].typeNumForTest(0, 500);
                            java.util.List<ImageView.RegShape> shs = pvV[0].imageView().regions();
                            if (shs.size() > 1) gNum[0] = shs.get(1).g.clone();
                        }
                    });
                    boolean numOk = gNum[0] != null && gNum[0][0] == 500 && gNum[0][2] == 1956 && gNum[0][3] == 98;
                    /* ③ 属性（改名/停用/绑定）+ [保存全部] → 盘上应如实回写 */
                    final String[] apErrB = new String[1];
                    mainSync(new Runnable() {
                        public void run() {
                            apErrB[0] = pvV[0].applyPropsForTest(1, "offx", false, "op1", "press");
                        }
                    });
                    String apErr = apErrB[0];
                    String saveMsg2 = "";
                    try { saveMsg2 = pvV[0].saveAllRegionsSync(); } catch (Throwable t2) { saveMsg2 = "EX:" + t2; }
                    /* ④ 删除 r1（盘上件）→ 立即写盘：r1 行消失，offx 保留 */
                    final ImageView.RegShape[] delSh = new ImageView.RegShape[1];
                    mainSync(new Runnable() {
                        public void run() {
                            java.util.List<ImageView.RegShape> shs = pvV[0].imageView().regions();
                            if (shs.size() > 0) delSh[0] = shs.get(0);
                        }
                    });
                    String derr = delSh[0] != null ? pvV[0].deleteShapeFileSync(delSh[0]) : "no-shape";
                    final String edel = derr;
                    if (edel == null) {
                        mainSync(new Runnable() {
                            public void run() { pvV[0].removeShapeAndRefresh(delSh[0]); }
                        });
                    }
                    byte[] vb = new byte[(int) vfile.length()];
                    java.io.FileInputStream vi2 = new java.io.FileInputStream(vfile);
                    vi2.read(vb); vi2.close();
                    String vTxt = new String(vb, "UTF-8");
                    final int[] cntV = new int[1];
                    mainSync(new Runnable() {
                        public void run() { cntV[0] = pvV[0].imageView().regions().size(); }
                    });
                    boolean propOk = vTxt.contains("region offx 0 500 686 2456 784 0")
                            && vTxt.contains("bind offx op1 press");
                    boolean delOk = edel == null && !vTxt.contains("region r1 ") && !vTxt.contains("bind r1 ")
                            && cntV[0] == 1;
                    final int[][] sp13B = new int[1][];
                    mainSync(new Runnable() {
                        public void run() {
                            pvV[0].selectRegionForTest(0);
                            sp13B[0] = pvV[0].canvasSpaceForTest();
                        }
                    });
                    int[] sp2 = sp13B[0];
                    /* ⑥ 无帧直开网格画布（不一定要设置背景）：空帧库 → 竖屏网格 + 形状就绪 */
                    java.io.File nfdir = new java.io.File(scratch, "noframes");
                    nfdir.mkdirs();
                    FrameStore.setDir(nfdir);
                    FrameStore.flashShot = null;
                    final CountDownLatch l10 = new CountDownLatch(1);
                    final VisualPage[] pvN = new VisualPage[1];
                    mainSync(new Runnable() {
                        public void run() {
                            VisualPage.Session sesN = new VisualPage.Session();
                            sesN.mode = VisualPage.MODE_REGIONS;
                            VisualPage p = new VisualPage(ctx, sesN, null);
                            p.attach(new Runnable() { public void run() { l10.countDown(); } });
                            pvN[0] = p;
                        }
                    });
                    boolean nfOk = l10.await(10, TimeUnit.SECONDS);
                    final int[][] spN = new int[1][];
                    final int[] cntN = new int[1];
                    mainSync(new Runnable() {
                        public void run() {
                            spN[0] = pvN[0].canvasSpaceForTest();
                            cntN[0] = pvN[0].imageView().regions().size();
                        }
                    });
                    nfOk = nfOk && spN[0] != null && spN[0][2] == 1 && spN[0][0] == 1440 && spN[0][1] == 3168 && cntN[0] == 1;
                    FrameStore.setDir(scratch);
                    v13ok = r9 && orientOk && numOk && (apErr == null) && propOk && delOk && nfOk;
                    v13msg = "方向=" + orientOk + " 数值=" + numOk + " 属性=" + (apErr == null) + "/" + propOk
                            + " 删除=" + delOk + " 无帧网格=" + nfOk + "(sp=" + (spN[0] != null ? (spN[0][0] + "x" + spN[0][1] + " g" + spN[0][2]) : "null") + " cnt=" + cntN[0] + ")"
                            + " save=" + saveMsg2 + " del=" + (edel == null ? "ok" : edel)
                            + " cnt=" + cntV[0] + " sp=" + (sp2 != null ? (sp2[0] + "x" + sp2[1] + " g" + sp2[2] + " off" + sp2[3]) : "null");
                    if (r9) {
                        Throwable e9 = mainSync(new Runnable() {
                            public void run() { renderView(ctx, pvV[0].root, "edit_regions_grid.png"); }
                        });
                        if (e9 != null) { v13ok = false; v13msg += " render:" + e9; }
                    }
                    /* v13.3：双档溢出核（竖屏现布局 + 宽结构横屏 3168×1200 压力值）——防「按钮被裁看不见」 */
                    final List<String> ovV = new ArrayList<String>();
                    mainSync(new Runnable() {
                        public void run() { assertOverflow(pvV[0].root, "竖屏", ovV, ctx); }
                    });
                    final CountDownLatch l11 = new CountDownLatch(1);
                    final VisualPage[] pvW = new VisualPage[1];
                    mainSync(new Runnable() {
                        public void run() {
                            VisualPage.wideOverrideForTest = Boolean.TRUE;
                            VisualPage.Session sesW = new VisualPage.Session();
                            sesW.mode = VisualPage.MODE_REGIONS;
                            VisualPage p = new VisualPage(ctx, sesW, null);
                            p.attach(new Runnable() { public void run() { l11.countDown(); } });
                            pvW[0] = p;
                        }
                    });
                    boolean r11 = l11.await(10, TimeUnit.SECONDS);
                    final List<String> ovW = new ArrayList<String>();
                    mainSync(new Runnable() {
                        public void run() {
                            pvW[0].selectRegionForTest(0);
                            renderViewWh(ctx, pvW[0].root, "edit_regions_wide.png", 3168, 1200);
                            assertOverflow(pvW[0].root, "横屏", ovW, ctx);
                            VisualPage.wideOverrideForTest = null;
                        }
                    });
                    mainSync(new Runnable() {
                        public void run() {
                            pvV[0].selectRegionForTest(-1);            /* v13.5：未选中态——验证描述行完整不被裁 */
                            renderView(ctx, pvV[0].root, "edit_regions_desc.png");
                            assertOverflow(pvV[0].root, "描述态", ovV, ctx);
                        }
                    });
                    boolean fitOk = r11 && ovV.isEmpty() && ovW.isEmpty();
                    v13ok = v13ok && fitOk;
                    v13msg += " 溢出核=" + (fitOk ? "无(竖/横)"
                            : (!ovV.isEmpty() ? ovV.get(0) : (!ovW.isEmpty() ? ovW.get(0) : "attach超时")));
                } catch (Throwable t9) {
                    v13ok = false;
                    v13msg = "EX:" + t9;
                } finally {
                    RegionDraw.readOverride = oldRO3;
                    RegionDraw.pathOverride = oldPO3;
                }
                rec(out, logFile, (v13ok ? "[OK] " : "[FAIL] ")
                        + "画布 v13（方向视图/网格 + 数值编辑 + 属性 + 删除立即写盘 + edit_regions_grid.png） " + v13msg);
            }
        } catch (Throwable e) {
            rec(out, logFile, "[FAIL] v7.1 修复轮异常: " + e);
        } finally {
            FrameStore.setDir(null);
            FrameStore.flashShot = null;
            Assets.dir = oldADir;
            deleteRec(scratch);
        }
    }


}