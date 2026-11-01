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
        int total = (doData ? 6 : 0) + (doRender ? 5 : 0);   // 渲染 4 页 + 图标/对话框预览 1 步
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
        String[] expect = {"新建区域", "新建操作", "新建空方案", "核心控制"};
        String[] pname = {"区域", "操作", "方案", "状态"};
        for (int i = 0; i < 4; i++) {
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
                Ui.Dlg f = Ui.dialog(ctx, "编辑区域 r1");
                f.body.addView(Ui.label(ctx, "id（[A-Za-z0-9_-]，1..15）"));
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
                        "屏帧 1440×3168 · 坐标 0..1439 / 0..3167");
                renderWrap(ctx, re.root, "edit_region.png", Ui.BG);
                Ops.Step sv = new Ops.Step();
                sv.type = Ops.T_TAP;
                sv.a1 = 720; sv.a2 = 1584; sv.ms = 50;
                java.util.List<String> idsList = new java.util.ArrayList<String>();
                idsList.add("c1");
                idsList.add("s1");
                StepEdit se = StepEdit.build(ctx, sv, idsList, idsList, null, null, new String[Ops.VAR_N]);
                renderWrap(ctx, se.root, "edit_step.png", Ui.BG);
                // 3.0b) 计算步 / 找图步编辑页预览（v6 新编辑器）
                Ops.Step sc = new Ops.Step();
                sc.type = Ops.T_CALC; sc.a1 = 0; sc.ref = "dx"; sc.expr = "atan2(tuy-tdy, tux-tdx)";
                StepEdit se2 = StepEdit.build(ctx, sc, idsList, idsList, null, null, new String[Ops.VAR_N]);
                renderWrap(ctx, se2.root, "edit_calc.png", Ui.BG);
                Ops.Step sf = new Ops.Step();
                sf.type = Ops.T_FINDIMAGE; sf.a1 = 8; sf.ref = "tpl1"; sf.expr = "c1"; sf.a4 = Ops.C_CONT;
                StepEdit se3 = StepEdit.build(ctx, sf, idsList, idsList, null, null, new String[Ops.VAR_N]);
                renderWrap(ctx, se3.root, "edit_find.png", Ui.BG);
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
            rec(out, logFile, "[OK] 对话框/编辑页预览：dlg_confirm / dlg_form / edit_region / edit_step / move_dialog / step_flow；符号探针 glyphs.png + mono 探针 mono_probe.png + 光晕探针 glow_probe.png + 品牌探针 brand_probe.png + 步骤预览 edit_calc / edit_find");
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
}
