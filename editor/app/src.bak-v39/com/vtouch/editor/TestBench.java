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
 * 批次（mode=all）：数据自检（su/往返/区域/操作/校验/写盘/ops v8/跨方案复制/控制流/块分支插入/复制与收起/素材/匹配/
 *   抓帧/看图器/会话页/修复轮）+ 五页渲染与布局断言 + 预览探针（步数随版本增长，见 run() 清单）。
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
        int total = (doData ? 19 : 0) + (doRender ? 5 : 0);   // 数据 19 步（v35 +T6 双视图）+ 渲染 4 页 + 图标/对话框预览 1 步
        List<String> out = new ArrayList<String>();
        File logFile = new File(ctx.getExternalFilesDir(null), "test_run.txt");
        try {
            FileOutputStream fo = new FileOutputStream(logFile, false);
            fo.write(("== vtouch 测试台 " + ts() + " mode=" + m + " ==\n").getBytes("UTF-8"));
            fo.close();
        } catch (Throwable ig) { }

        VisualPage.autoWriteSuppressedForTest = true;   /* v22：套件内画布不自动写盘（用例显式驱动；自动写盘用例定点放开） */
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
                if (cb != null) cb.step("ops v8 存储", ++idx, total);
                opsV8Checks(out, logFile);
                if (cb != null) cb.step("跨方案复制", ++idx, total);
                crossCopyChecks(out, logFile);
                if (cb != null) cb.step("控制流模型", ++idx, total);
                ctrlFlowChecks(out, logFile);
                if (cb != null) cb.step("控制流 v6B", ++idx, total);
                ctrlFlowV6BChecks(out, logFile);
                if (cb != null) cb.step("块分支插入", ++idx, total);
                branchInsChecks(out, logFile);
                if (cb != null) cb.step("复制与收起", ++idx, total);
                copyFoldChecks(out, logFile);
                if (cb != null) cb.step("T6 双视图", ++idx, total);
                dualViewChecks(ctx, out, logFile);
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
        VisualPage.autoWriteSuppressedForTest = false;   /* 收工恢复（防影响真实使用） */

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
                    + "step 24 1 2 3 4 5 r 7 8 x y z\n"     // v6F：24 = 未知类型（逐字保留用例；19/20/21/22/23 已升为已知）
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

            /* v21 新建草稿口径：创建只查名（validateName 放行）；空操作在 [完成]/[保存] 仍被 validate 拦 */
            Ops.Op draftOk = new Ops.Op();
            draftOk.name = "opj";
            boolean v13 = Ops.validateName(draftOk, of, null) == null;
            Ops.Op draftDup = new Ops.Op();
            draftDup.name = "opk";
            boolean v14 = Ops.validateName(draftDup, of, null) != null;
            boolean v15 = Ops.validate(draftOk, of, null) != null;

            boolean all = v1 && v2 && v3 && v4 && v5 && v6 && v7 && v8 && v9 && v9b && v10 && v11 && v12
                    && n1 && v13 && v14 && v15;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "校验与命名："
                    + "矩形合法=" + v1 + " 半径负拒=" + v2 + " 重名拒=" + v3 + " 超界拒=" + v4
                    + " 操作合法=" + v5 + " 跳转越界拒=" + v6 + " 条件跳转越界拒=" + v7
                    + " 计算合法=" + v8 + " 表达式语法拒=" + v9 + " 未知名拒=" + v9b
                    + " 找图阈值拒=" + v10 + " 找色单点带点集拒=" + v11 + " expr挂错类型拒=" + v12
                    + " 命名规则=" + n1 + " 草稿名过=" + v13 + " 草稿重名拒=" + v14 + " 空操作拦=" + v15);
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

    // ---------------------------------------------------------------- ops v8 存储（T4.1）

    /** ops v8 存储（T4.1；格式规范 = docs/VTSTORE_V8.md）：scratch 根的元数据+progs 读写往返 /
     *  镜像同步与对账（互证 + 篡改可见 + 修复）/ .bak-v8 备份 / 缺产物负例；收尾做真实数据只读对账。 */
    private static void opsV8Checks(List<String> out, File logFile) {
        String scratch = "/data/local/tmp/vte-v8scratch";
        try {
            Su.run("rm -rf '" + scratch + "'");
            OpsV8.baseOverride = scratch;
            String schemeB = OpsV8.schemeDir("T8T");
            Su.run("mkdir -p '" + schemeB + "'");

            /* 真编译器产一件当产物：存储层往返的字节 = 编译器产出 */
            VtCompiler.R cr = VtCompiler.compile("点按 300 400 300\n等待 200");
            byte[] vtbc = cr.err == null ? VtBc.write(cr.blob) : new byte[0];

            /* 1) 元数据 + progs 写 → 读回 → 完整性校验 */
            OpsV8.MetaFile mf = new OpsV8.MetaFile();
            OpsV8.Rec r1 = new OpsV8.Rec();
            r1.name = "ztp1"; r1.gate = "-"; r1.autooff = 0;
            r1.hash = OpsV8.fnv8(vtbc); r1.size = vtbc.length;
            mf.recs.add(r1);
            boolean wm = OpsV8.writeMeta(OpsV8.liveOps(), mf).ok();
            boolean wp = OpsV8.writeProg(OpsV8.base(), "ztp1", vtbc).ok();
            OpsV8.MetaFile rb = OpsV8.readMeta(OpsV8.liveOps());
            byte[] rp = OpsV8.readProg(OpsV8.base(), "ztp1");
            boolean rt = cr.err == null && wm && wp && rb != null && rb.recs.size() == 1
                    && "ztp1".equals(rb.recs.get(0).name) && vbEq(rp, vtbc)
                    && OpsV8.checkArtifact(rb.recs.get(0), rp) == null;

            /* 2) 镜像（ops.conf 复制 + progs 目录同步）→ 对账一致 → 篡改可见 → 再同步修复 */
            Su.run("cp -f '" + OpsV8.liveOps() + "' '" + schemeB + "/ops.conf'");
            boolean mir = OpsV8.mirrorProgs(OpsV8.base(), schemeB).ok() && OpsV8.audit("T8T").isEmpty();
            OpsV8.writeProg(schemeB, "ztp1", new byte[]{1, 2, 3});
            List<String> d1 = OpsV8.audit("T8T");
            boolean tam = d1.size() == 1 && d1.get(0).equals("异 progs/ztp1.vtbc");
            boolean fix = OpsV8.mirrorProgs(OpsV8.base(), schemeB).ok() && OpsV8.audit("T8T").isEmpty();

            /* 3) .bak-v8 备份（文件 + progs 目录） */
            OpsV8.backupV8(OpsV8.base());
            boolean bk = Su.fileExists(OpsV8.base() + "/ops.conf.bak-v8")
                    && Su.fileExists(OpsV8.base() + "/progs.bak-v8/ztp1.vtbc");

            /* 4) 负例：缺产物的名字读回 null */
            boolean miss = OpsV8.readProg(OpsV8.base(), "不存在") == null;

            /* 5) 真实数据只读对账（live vs schemes/<current>；差异照报不判 FAIL——漂移非代码错） */
            OpsV8.baseOverride = null;
            String cur = Schemes.current();
            List<String> real = cur.isEmpty() ? null : OpsV8.audit(cur);

            boolean all = rt && mir && tam && fix && bk && miss && real != null;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "ops v8 存储：往返=" + rt + " 镜像对账=" + mir
                    + " 篡改可见=" + tam + " 修复=" + fix + " 备份=" + bk + " 缺产物 null=" + miss
                    + " 真实对账=" + (real == null ? "跳过(无current)" : (real.isEmpty() ? "一致" : "差异" + real.size() + real)));
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] ops v8 存储异常: " + t);
        } finally {
            OpsV8.baseOverride = null;
            Su.run("rm -rf '" + scratch + "'");
        }
    }

    /** 字节相等（测试台内部小工具）。 */
    private static boolean vbEq(byte[] a, byte[] b) {
        if (a == null || b == null) return a == b;
        if (a.length != b.length) return false;
        for (int i = 0; i < a.length; i++) if (a[i] != b[i]) return false;
        return true;
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

            /* 用例5（v6C 评审 F2）：控制流判定行引用扫描 —— 14 / 12 模式3 / 19 的 ref·expr 进依赖与改名 */
            Ops.Op co = new Ops.Op();
            co.name = "ctx";
            co.gate = "-";
            co.steps.add(bstep(Ops.T_LOOP, 3, 0, 0, 0, 0, "s1", "", 0, 0));           // 12m3·开关判断 s1
            co.steps.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 0, 100, 200, 0, "r3", "", 0, 0));   // 19·区域判断 r3
            co.steps.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 2, 8, 1, 0, "tpl", "r9", 0, 0));   // 19·找图（区域限定 r9）
            co.steps.add(bstep(Ops.T_IF, 0, 0, 1, 1, 0, "s1", "", 0, 0));            // 14·区域判断 s1
            java.util.List<String> refs5 = CrossCopy.opRegionRefs(co);
            boolean c5 = refs5.contains("s1") && refs5.contains("r3") && refs5.contains("r9");
            CrossCopy.remapOpRegionRefs(co, "s1", "sX");
            CrossCopy.remapOpRegionRefs(co, "r3", "rX");
            CrossCopy.remapOpRegionRefs(co, "r9", "rY");
            boolean r5ok = "sX".equals(co.steps.get(0).ref) && "rX".equals(co.steps.get(1).ref)
                    && "rY".equals(co.steps.get(2).expr) && "sX".equals(co.steps.get(3).ref)
                    && "tpl".equals(co.steps.get(2).ref);   // 找图模板名不受区域改名影响

            /* 用例6（v35）：v8 操作——脚本级依赖携带 + 改名改写重编译 + 产物清单 */
            final String v8src = "如果 在区域(\"r9\", tux, tuy) {\n    点按 100 200 50\n}\n";
            VtCompiler.R c6r = VtCompiler.compile(v8src);
            final byte[] c6b = c6r.err == null ? VtBc.write(c6r.blob) : new byte[0];
            final String v8srcR = "#vtouch-regions v2\n#frame 0 1440 3168\n"
                    + "region r9 0 100 100 200 200 1\n"
                    + "bind r9 v8op down\n"
                    + "kind r9 1\n";
            final String v8srcO = "#vtouch-ops v8\n"
                    + "op v8op gate - autooff 0 prog " + OpsV8.fnv8(c6b) + " " + c6b.length + "\n";
            CrossCopy.Vts vp = new CrossCopy.Vts() {
                public String get(String n) { return "v8op".equals(n) ? v8src : null; }
            };
            /* 6a：复制区域 r9（目标空）→ 携带 v8op（脚本级引用扫描）+ verbatim 产物条目 */
            CrossCopy.Inspect i6 = CrossCopy.inspect(true, "r9", v8srcR, v8srcO,
                    "#vtouch-regions v2\n#frame 0 1440 3168\n", "#vtouch-ops v8\n", vp);
            CrossCopy.Rendered d6 = CrossCopy.render(i6, false);
            boolean c6a = i6.err == null && i6.carries.contains("操作 v8op")
                    && d6.artifacts.size() == 1 && "v8op".equals(d6.artifacts.get(0).name)
                    && d6.artifacts.get(0).bytes == null && "v8op".equals(d6.artifacts.get(0).srcFrom);
            /* 6b：目标已有 r9 → 冲突改名 r9-2 → 携带的 v8op 脚本引用改写 + 重编译（哈希更新） */
            CrossCopy.Inspect i7 = CrossCopy.inspect(true, "r9", v8srcR, v8srcO,
                    "#vtouch-regions v2\n#frame 0 1440 3168\nregion r9 0 9 9 9 9 1\n", "#vtouch-ops v8\n", vp);
            CrossCopy.Rendered d7 = CrossCopy.render(i7, true);
            boolean c6bOk = false;
            try {
                Ops.File o7 = Ops.File.parse(d7.newDstOps);
                Ops.Op nv = o7.find("v8op");
                CrossCopy.Artifact a7 = d7.artifacts.size() == 1 ? d7.artifacts.get(0) : null;
                c6bOk = i7.err == null && "r9-2".equals(d7.appliedName)
                        && a7 != null && a7.bytes != null && a7.srcText != null
                        && a7.srcText.contains("在区域(\"r9-2\"")
                        && nv != null && nv.progHash != null && nv.progHash.equals(OpsV8.fnv8(a7.bytes))
                        && nv.progSize == a7.bytes.length;
            } catch (Throwable ig) { }

            boolean all = c1 && r1ok && c2 && r2ok && r3ok && c4 && c5 && r5ok && c6a && c6bOk;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "跨方案复制："
                    + "无冲突直带=" + (c1 && r1ok) + " 冲突改名+改写=" + (c2 && r2ok)
                    + " 覆盖替换=" + r3ok + " 基帧警告=" + c4 + " 控制流行引用=" + (c5 && r5ok)
                    + " v8 携带+改写=" + (c6a && c6bOk));
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 跨方案复制异常: " + t);
        }
    }

    // ---------------------------------------------------------------- T6 双视图（v35）

    /** 等文件内容出现子串（跨线程写盘对账用）；返回最后读到的内容。 */
    private static String waitRead(String path, String contains, int ms) {
        long t0 = System.currentTimeMillis();
        String s = null;
        while (System.currentTimeMillis() - t0 < ms) {
            s = Su.read(path);
            if (s != null && s.contains(contains)) return s;
            try { Thread.sleep(150); } catch (Exception ig) { }
        }
        return s;
    }

    /** T6 双视图（v35）：投影往返核 / 侧线与 prepare / OpsPage 端到端（打开路由→两向转换→保存→读回对账）/ live 只读核。 */
    private static void dualViewChecks(final Context ctx, List<String> out, File logFile) {
        String scratch = "/data/local/tmp/vte-t6scratch";
        int nTot = 0, nOk = 0;
        try {
            Su.run("rm -rf '" + scratch + "'");
            OpsV8.baseOverride = scratch;
            Su.run("mkdir -p '" + scratch + "/progs'");

            /* a) 投影往返核（合成语料：真实 op1 13 步 / 组合条件 / 错误行 / 兼容语法拒绝） */
            String op1 = "如果 在区域(\"c1\", tux, tuy) {\n} 否则 {\n    循环 时长=2000 {\n        如果 找色(\"lb1\") {\n"
                    + "            e1 = tdx + cos(atan2(tuy - tdy, tux - tdx)) * 200\n"
                    + "            e2 = tdy + sin(atan2(tuy - tdy, tux - tdx)) * 200\n"
                    + "            滑动 tdx tdy e1 e2 时长=100 停1=30\n"
                    + "            点按 154 1978 50\n            跳出\n        }\n        等待 50\n    }\n}\n";
            VtToSteps.R pa = VtToSteps.project(op1);
            boolean a1 = pa.err == null && pa.exact && op1.equals(pa.src) && pa.op.steps.size() == 13;
            String comp = "如果 开关开(\"s1\") && !开关开(\"s2\") || 开关开(\"s3\") {\n    等待 100\n}\n";
            VtToSteps.R pb = VtToSteps.project(comp);
            boolean a2 = pb.err == null && pb.exact && pb.op.steps.size() == 5
                    && pb.op.steps.get(0).type == Ops.T_IF && pb.op.steps.get(1).type == Ops.T_CLAUSE
                    && Ops.structureErr(pb.op.steps) == null;
            VtToSteps.R pc = VtToSteps.project("循环 {\n");
            boolean a3 = pc.err != null && pc.err.contains("第 1 行");
            VtToSteps.R pd = VtToSteps.project("等待 100\n跳转 第1行\n");
            boolean a4 = pd.err != null && pd.err.contains("跳转");

            /* b) 侧线 + prepare 核 */
            String wp = ScriptStore.withPragma("等待 100\n", "块");
            boolean b1 = "#模式 块\n等待 100\n".equals(wp) && "块".equals(ScriptStore.modeOf(wp))
                    && ScriptStore.modeOf("等待 100\n") == null;
            ScriptStore.Save sv = ScriptStore.prepare("等待 100\n", "脚本");
            boolean b2 = sv.err == null && sv.bytes != null && "#模式 脚本\n等待 100\n".equals(sv.src);

            /* c) OpsPage 端到端（scratch；conf 先落盘再装载） */
            ScriptStore.Save c0 = ScriptStore.prepare("#模式 脚本\n等待 100\n", "脚本");
            OpsV8.writeSrc(scratch, "v8op", "#模式 脚本\n等待 100\n");
            OpsV8.writeProg(scratch, "v8op", c0.bytes);
            OpsV8.writeSrc(scratch, "v8b", "等待 100\n");
            OpsV8.writeProg(scratch, "v8b", c0.bytes);
            String conf = "#vtouch-ops v8\n"
                    + "op v8op gate - autooff 0 prog " + OpsV8.fnv8(c0.bytes) + " " + c0.bytes.length + "\n"
                    + "op v8b gate - autooff 0 prog " + OpsV8.fnv8(c0.bytes) + " " + c0.bytes.length + "\n";
            Su.writeAtomic(scratch + "/ops.conf", conf.getBytes("UTF-8"));

            OpsPage pg = new OpsPage(ctx);
            pg.build();
            pg.pathOverride = scratch + "/ops.conf";
            pg.applyLoad(conf, new ArrayList<String>(), new ArrayList<String>(), null, null,
                    new ArrayList<Regions.Region>());

            /* c1：打开路由 / 模式推导——v8op 侧线=脚本；v8b 可投影=块 */
            String e1 = pg.startOpForTest(0);
            boolean c1 = e1 == null && "脚本".equals(pg.editModeForTest()) && pg.scriptEditorForTest() != null;
            pg.closeEditor();
            String e2 = pg.startOpForTest(1);
            boolean c2 = e2 == null && "块".equals(pg.editModeForTest()) && pg.scriptEditorForTest() == null;
            pg.closeEditor();

            /* c3：脚本视图改文本 → [保存] → .vts 读回 + conf 产物对账（轮询等待落盘） */
            pg.startOpForTest(0);
            pg.scriptEditorForTest().setTextForTest("#模式 脚本\n等待 150\n");
            String ce3 = pg.commitEdit();
            String vts3 = waitRead(OpsV8.srcPath(scratch, "v8op"), "等待 150", 4000);
            boolean confOk3 = false;
            for (int k = 0; k < 20; k++) {
                OpsV8.MetaFile mx = OpsV8.readMeta(scratch + "/ops.conf");
                byte[] bx = OpsV8.readProg(scratch, "v8op");
                if (mx != null && mx.find("v8op") != null && OpsV8.checkArtifact(mx.find("v8op"), bx) == null) { confOk3 = true; break; }
                try { Thread.sleep(150); } catch (Exception ig) { }
            }
            boolean c3 = ce3 == null && vts3 != null && vts3.equals("#模式 脚本\n等待 150\n") && confOk3;
            try { Thread.sleep(250); } catch (Exception ig) { }   // 让主线程收尾渲染错峰

            /* c4：脚本 → 块（子集内）→ [完成]（块视图保存 = 规范重生成 + 侧线=块） */
            pg.startOpForTest(0);
            String ce4a = pg.switchToBlock();
            boolean c4a = ce4a == null && "块".equals(pg.editModeForTest());
            String ce4 = pg.commitEdit();
            String vts4 = waitRead(OpsV8.srcPath(scratch, "v8op"), "#模式 块", 4000);
            boolean c4 = c4a && ce4 == null && vts4 != null && vts4.equals("#模式 块\n等待 150\n");
            try { Thread.sleep(250); } catch (Exception ig) { }

            /* c5：脚本 → 块 非子集拒绝（函数）+ 非法脚本保存被拒（不写盘） */
            pg.startOpForTest(0);
            String c5s = pg.switchToScript();
            boolean c5open = c5s == null && "脚本".equals(pg.editModeForTest());
            pg.scriptEditorForTest().setTextForTest("函数 f() {\n    返回 1\n}\n");
            String ce5 = pg.switchToBlock();
            boolean c5 = c5open && ce5 != null && ce5.contains("函数") && "脚本".equals(pg.editModeForTest());
            /* c5b：确定非法的脚本 → [保存] 被拒（报行号、不写盘） */
            pg.scriptEditorForTest().setTextForTest("#模式 脚本\n点按 50\n循环 {\n");
            String ce5b = pg.commitEdit();
            String vts5 = Su.read(OpsV8.srcPath(scratch, "v8op"));
            boolean c5b = ce5b != null && ce5b.contains("第") && vts5 != null && vts5.contains("等待 150");
            pg.closeEditor();

            /* c6：块视图 [完成] 保存（v8b 无侧线 → 块；生成脚本 + 侧线=块） */
            pg.startOpForTest(1);
            String ce6 = pg.commitEdit();
            String vts6 = waitRead(OpsV8.srcPath(scratch, "v8b"), "#模式 块", 4000);
            boolean c6 = ce6 == null && vts6 != null && vts6.equals("#模式 块\n等待 100\n");
            try { Thread.sleep(250); } catch (Exception ig) { }

            /* e) 格式化核（v36）：规范重排 + 注释保留 + 幂等 + [格式化] UI 路径 + 缩进键/回车自动缩进 */
            String messy = "等待 100\n  如果 开关开(\"s1\") {\n点按 1 2\n# 说明\n}\n";
            ScriptStore.Fmt fm = ScriptStore.format(messy);
            String wantF = "等待 100\n如果 开关开(\"s1\") {\n    点按 1 2\n    # 说明\n}\n";
            boolean ef1 = fm.err == null && wantF.equals(fm.src);
            ScriptStore.Fmt fm2 = ScriptStore.format(fm.src == null ? "" : fm.src);
            boolean ef2 = fm2.err == null && fm.src != null && fm.src.equals(fm2.src);
            pg.startOpForTest(0);
            String eo = pg.switchToScript();
            pg.scriptEditorForTest().setTextForTest(messy);
            String ef = pg.formatScript();
            ScriptEdit sed2 = pg.scriptEditorForTest();
            boolean ef3 = eo == null && ef == null && sed2 != null && wantF.equals(sed2.textForTest());
            String ec = pg.commitEdit();
            String vtsE = waitRead(OpsV8.srcPath(scratch, "v8op"), "# 说明", 4000);
            boolean ef4 = ec == null && vtsE != null && vtsE.startsWith("#模式 脚本\n") && vtsE.contains("    点按 1 2\n");
            try { Thread.sleep(250); } catch (Exception ig) { }
            ScriptEdit sx = ScriptEdit.build(ctx, "", "门控 无 · 自动关 关", null);
            sx.setTextForTest("  等待 100\n");
            sx.indentForTest(true);
            boolean ef5 = sx.textForTest().equals("等待 100\n");
            sx.setTextForTest("等待 100\n");
            sx.indentForTest(false);
            boolean ef6 = sx.textForTest().equals("    等待 100\n");
            sx.setTextForTest("如果 开关开(\"s1\") {");
            sx.cursorEndForTest();
            String t2i = sx.typeProbeForTest("\n");
            boolean ef7 = t2i.equals("如果 开关开(\"s1\") {\n    ");
            boolean eOk = ef1 && ef2 && ef3 && ef4 && ef5 && ef6 && ef7;

            /* f) 输入提示核（v37）：命令帮助条 / 前缀候选+点选补全 / 串内抑制 / 行首回退 */
            ScriptEdit sh = ScriptEdit.build(ctx, "", "门控 无 · 自动关 关", null);
            sh.setTextForTest("点按 100\n");
            sh.cursorToForTest(2);
            String ht1 = sh.hintTextForTest();
            boolean hf1 = ht1 != null && ht1.contains("点按") && ht1.contains("点击");
            sh.setTextForTest("找");
            sh.cursorEndForTest();
            boolean hf2 = sh.hintCandCountForTest() == 3;
            sh.tapCandForTest(0);
            boolean hf3 = sh.textForTest().startsWith("找图");
            sh.setTextForTest("提示 \"等待\n");
            sh.cursorToForTest(6);
            boolean hf4 = sh.hintTextForTest() == null;
            sh.setTextForTest("  等待 100\n");
            sh.cursorToForTest(8);
            String ht2 = sh.hintTextForTest();
            boolean hf5 = ht2 != null && ht2.contains("暂停指定毫秒");
            /* v37b：候选补全 / 命令键 = 插入参数模板（首占位选中；块级缩进体随行） */
            sh.setTextForTest("点");
            sh.cursorEndForTest();
            sh.tapCandForTest(0);
            boolean hf6 = sh.textForTest().equals("点按 100 200 50") && sh.selForTest().equals("3:6");
            sh.setTextForTest("循");
            sh.cursorEndForTest();
            sh.tapCandForTest(0);
            boolean hf7 = sh.textForTest().equals("循环 3 {\n    \n}") && sh.selForTest().equals("3:4");
            sh.setTextForTest("    循");
            sh.cursorToForTest(5);
            sh.tapCandForTest(0);
            boolean hf8 = sh.textForTest().equals("    循环 3 {\n        \n    }");
            sh.setTextForTest("");
            sh.cursorEndForTest();
            sh.insertKeyForTest("等待");
            boolean hf9 = sh.textForTest().equals("等待 100") && sh.selForTest().equals("3:6");
            boolean hOk = hf1 && hf2 && hf3 && hf4 && hf5 && hf6 && hf7 && hf8 && hf9;

            /* g) 取点回填核（v39）：辅助条两枚取点键 + 光标插入 + 活会话路由 */
            ScriptEdit sg = ScriptEdit.build(ctx, "", "门控 无 · 自动关 关", null);
            final int[] gFire = new int[1];
            sg.enablePickKeys(new Runnable() { public void run() { gFire[0] |= 1; } },
                    new Runnable() { public void run() { gFire[0] |= 2; } });
            sg.insertKeyForTest("帧上取点");
            boolean gk1 = (gFire[0] & 1) != 0;
            sg.insertKeyForTest("屏幕取点");
            boolean gk2 = (gFire[0] & 2) != 0;
            sg.setTextForTest("点按 ");
            sg.cursorEndForTest();
            sg.insertPickedPoint(540, 1584);
            boolean gk3 = sg.textForTest().equals("点按 540 1584 ");
            sg.setTextForTest("等待 100\n");
            sg.cursorToForTest(3);                       /* 行中插入：不吞后续字符 */
            sg.insertPickedPoint(11, 22);
            boolean gk4 = sg.textForTest().equals("等待 11 22 100\n");
            pg.startOpForTest(0);
            if ("块".equals(pg.editModeForTest())) pg.switchToScript();
            boolean gk5 = pg.scriptEditorForTest() != null
                    && pg.insertScriptPick(66, 77) && pg.scriptEditorForTest().textForTest().contains("66 77 ");
            pg.closeEditor();
            boolean gk6 = !pg.insertScriptPick(1, 2);
            boolean gOk = gk1 && gk2 && gk3 && gk4 && gk5 && gk6;

            /* d) live 只读核：真实 progs 全量投影（正例=回稳，非子集只计数不判）+ [格式化] 恒等 */
            int nFmt = 0, nFmtBad = 0;
            Su.R ls = Su.run("ls /data/local/vtouch-runtime/progs/*.vts 2>/dev/null");
            if (ls.ok() && ls.out != null) {
                for (String ln : ls.out.split("\n")) {
                    String p = ln.trim();
                    if (!p.endsWith(".vts")) continue;
                    String s = Su.read(p);
                    if (s == null) continue;
                    nTot++;
                    VtToSteps.R pr = VtToSteps.project(s);
                    if (pr.err == null) {
                        nOk++;
                        ScriptStore.Fmt fd = ScriptStore.format(s);
                        if (fd.err == null && s.equals(fd.src)) nFmt++; else nFmtBad++;
                    }
                }
            }
            boolean d1 = nTot >= 1 && nFmtBad == 0;

            boolean all = a1 && a2 && a3 && a4 && b1 && b2 && c1 && c2 && c3 && c4 && c5 && c5b && c6 && eOk && hOk && gOk && d1;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "T6 双视图：投影往返=" + (a1 && a2) + " 拒绝词=" + (a3 && a4)
                    + " 侧线/prepare=" + (b1 && b2) + " 打开路由=" + (c1 && c2) + " 脚本保存对账=" + c3
                    + " 脚本→块保存=" + c4 + " 非子集拒=" + (c5 && c5b) + " 块保存=" + c6
                    + " 格式化=" + eOk + " 提示=" + hOk + " 取点键=" + gOk
                    + " live 只读（" + nOk + "/" + nTot + " 件可投影 · 格式化恒等 " + nFmt + "）=" + d1
                    + ((c5 && c5b) ? "" : " [c5:" + ce5 + " | c5b:" + ce5b + "]")
                    + (c6 ? "" : " [c6: ce6=" + ce6 + " vts6=" + (vts6 == null ? "null" : vts6.replace("\n", "\\n")) + "]")
                    + (eOk ? "" : " [e: " + ef1 + ef2 + ef3 + ef4 + ef5 + ef6 + ef7 + "]")
                    + (hOk ? "" : " [h: " + hf1 + hf2 + hf3 + hf4 + hf5 + hf6 + hf7 + hf8 + hf9 + "]")
                    + (gOk ? "" : " [g: " + gk1 + gk2 + gk3 + gk4 + gk5 + gk6 + "]"));
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] T6 双视图异常: " + t);
        } finally {
            OpsV8.baseOverride = null;
            Su.run("rm -rf '" + scratch + "'");
        }
    }

    // ---------------------------------------------------------------- 控制流模型（v12）

    /** 单操作 conf（默认头/名）→ 解析后 validate 的拒收原因（null = 过）。 */
    private static String ctrlValOf(String opName, String... stepLines) {
        StringBuilder sb = new StringBuilder("#vtouch-ops v5\nop " + opName + " gate - autooff 0\n");
        for (String l : stepLines) sb.append("step ").append(l).append('\n');
        Ops.File f = Ops.File.parse(sb.toString());
        Ops.Op o = f.find(opName);
        if (o == null) return "解析失败（op 未找到）";
        return Ops.validate(o, f, o);
    }

    private static boolean ctrlRej(String why, String word) {
        return why != null && why.contains(word);
    }

    /** 构造测试步骤（v12 块树渲染件用）。 */
    private static Ops.Step bstep(int type, int a1, int a2, int a3, int a4, int ms,
                                  String ref, String expr, int j1, int j2) {
        Ops.Step s = new Ops.Step();
        s.type = type; s.a1 = a1; s.a2 = a2; s.a3 = a3; s.a4 = a4; s.ms = ms;
        s.ref = ref; s.expr = expr; s.j1 = j1; s.j2 = j2;
        return s;
    }

    /** 控制流块模型（v12）：含块 conf 往返 + validate 正例/负例（配对/深度/字段域/跨块跳）+ 摘要渲染。 */
    private static void ctrlFlowChecks(List<String> out, File logFile) {
        try {
            StringBuilder why = new StringBuilder();

            /* a) 往返：含块 conf parse → serialize → 再 parse，结构行字段逐项一致 */
            String conf = "#vtouch-ops v5\n"
                    + "op zc1 gate - autooff 0\n"
                    + "step 12 0 3 0 0 0 - 0 0 -\n"
                    + "step 1 100 200 0 0 50 - 0 0 -\n"
                    + "step 13 0 0 0 0 0 - 0 0 -\n"
                    + "op zc2 gate - autooff 0\n"
                    + "step 12 3 2 200 0 0 tp1 0 0 -\n"
                    + "step 14 0 5 6 0 0 r9 0 0 -\n"
                    + "step 17 0 0 0 0 0 - 0 0 -\n"
                    + "step 15 0 0 0 0 0 - 0 0 -\n"
                    + "step 18 0 0 0 0 0 - 0 0 -\n"
                    + "step 16 0 0 0 0 0 - 0 0 -\n"
                    + "step 13 0 0 0 0 0 - 0 0 -\n";
            Ops.File f1 = Ops.File.parse(conf);
            Ops.File f2 = Ops.File.parse(f1.serialize());
            boolean round = f2.ops.size() == 2;
            if (round) {
                Ops.Op a = f1.find("zc2"), b = f2.find("zc2");
                round = a != null && b != null && a.steps.size() == 7 && b.steps.size() == 7;
                if (round) {
                    for (int i = 0; i < a.steps.size(); i++) {
                        Ops.Step sa = a.steps.get(i), sb = b.steps.get(i);
                        if (sa.type != sb.type || sa.a1 != sb.a1 || sa.a2 != sb.a2 || sa.a3 != sb.a3
                                || sa.a4 != sb.a4 || sa.ms != sb.ms || sa.j1 != sb.j1 || sa.j2 != sb.j2
                                || !sa.ref.equals(sb.ref) || !sa.expr.equals(sb.expr)) {
                            round = false;
                            why.append("往返字段不一致@").append(i + 1).append(' ');
                            break;
                        }
                    }
                }
            }
            /* 摘要/类型名渲染（块行） */
            boolean rend = Ops.typeName(Ops.T_LOOP).equals("循环")
                    && Ops.typeName(Ops.T_ENDIF).equals("条件块结束")
                    && Ops.typeName(Ops.T_BREAK).equals("跳出循环");
            if (rend && round) {
                rend = f1.find("zc1").steps.get(0).summary().equals("循环 · 次数 3")
                        && f1.find("zc2").steps.get(0).summary().startsWith("循环 · 到条件")
                        && f1.find("zc2").steps.get(1).summary().startsWith("如果 · 区域判断");
            }

            /* b) 正例：zc2 结构合法 → validate 过 */
            String v1 = Ops.validate(f1.find("zc2"), f1, f1.find("zc2"));
            boolean pos = v1 == null;
            if (!pos) why.append("正例被拒:").append(v1).append(' ');

            /* c) 负例：未闭合 / 交叉嵌套 / 跳出悬空 / 跨块跳 */
            boolean negOpen = ctrlRej(ctrlValOf("zc9", "12 0 2 0 0 0 - 0 0 -"), "循环未闭合");
            boolean negX = ctrlRej(ctrlValOf("zc9", "12 0 2 0 0 0 - 0 0 -", "14 0 7 8 0 0 r9 0 0 -",
                    "13 0 0 0 0 0 - 0 0 -", "16 0 0 0 0 0 - 0 0 -"), "块配对错误");
            boolean negBreak = ctrlRej(ctrlValOf("zc9", "17 0 0 0 0 0 - 0 0 -"), "跳出不在循环内");
            boolean negXj = ctrlRej(ctrlValOf("zc9", "12 0 2 0 0 0 - 0 0 -", "1 100 100 0 0 50 - 0 0 -",
                    "8 5 0 0 0 0 - 0 0 -", "13 0 0 0 0 0 - 0 0 -", "1 150 150 0 0 50 - 0 0 -"), "跳转跨越块边界");

            /* d) 深度：9 层嵌套拒（>8） */
            StringBuilder d9 = new StringBuilder();
            for (int i = 0; i < 9; i++) d9.append("12 0 2 0 0 0 - 0 0 -|");
            d9.append("1 1 1 0 0 10 - 0 0 -|");
            for (int i = 0; i < 9; i++) d9.append("13 0 0 0 0 0 - 0 0 -|");
            boolean negDeep = ctrlRej(ctrlValOf("zc9", d9.toString().split("\\|")), "嵌套超深");

            /* e) 字段域：次数 0 拒 / 时长 86400001 拒 */
            boolean negF1 = ctrlRej(ctrlValOf("zc9", "12 0 0 0 0 0 - 0 0 -", "13 0 0 0 0 0 - 0 0 -"), "循环次数越界");
            boolean negF2 = ctrlRej(ctrlValOf("zc9", "12 1 86400001 0 0 0 - 0 0 -", "13 0 0 0 0 0 - 0 0 -"), "循环时长越界");

            boolean all = round && rend && pos && negOpen && negX && negBreak && negXj && negDeep && negF1 && negF2
                    && why.length() == 0;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "控制流模型：往返=" + round + " 渲染=" + rend
                    + " 正例=" + pos + " 未闭合=" + negOpen + " 交叉嵌套=" + negX + " 跳出悬空=" + negBreak
                    + " 跨块跳=" + negXj + " 深度9=" + negDeep + " 字段域=" + (negF1 && negF2));
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 控制流模型异常: " + t);
        }
    }

    /** 控制流 v6B（多条件并或 / 终止 / N 层跳出）：往返 + validate 正负例 + setClauseGroup 保序 + 摘要（19/20）。 */
    private static void ctrlFlowV6BChecks(List<String> out, File logFile) {
        try {
            StringBuilder why = new StringBuilder();

            /* a) 往返：含 19（取反）/ 20 / 17 层数 的 conf parse → serialize → 再 parse，字段逐项一致 */
            String conf = "#vtouch-ops v5\n"
                    + "op zb1 gate - autooff 0\n"
                    + "step 14 2 8 1 1 0 t1 0 0 -\n"          // 如果·找图 t1（色校、取反）
                    + "step 19 1 1 0 0 0 s1 0 1 -\n"          // 或·开关判断 s1（取反）
                    + "step 19 0 0 100 200 0 r1 0 0 -\n"      // 且·区域判断 r1
                    + "step 19 0 4 -6 100 0 - 0 0 -\n"        // v6C：且·变量 fx > 100
                    + "step 1 100 200 0 0 50 - 0 0 -\n"
                    + "step 15 0 0 0 0 0 - 0 0 -\n"
                    + "step 20 1 0 0 0 0 - 0 0 -\n"           // 终止·中止退出
                    + "step 16 0 0 0 0 0 - 0 0 -\n"
                    + "op zb2 gate - autooff 0\n"
                    + "step 12 0 2 0 0 0 - 0 0 -\n"           // 外层循环 x2
                    + "step 12 3 2 8 0 0 t9 0 0 -\n"          // 内层·到条件·找图 t9
                    + "step 19 0 0 5 6 0 r9 0 0 -\n"          // 且·区域判断 r9
                    + "step 19 1 3 1 0 0 pts1 0 0 -\n"        // v6C：或·找色·多点（修复：模式=1 写回）
                    + "step 17 2 0 0 0 0 - 0 0 -\n"           // 跳出 2 层
                    + "step 13 0 0 0 0 0 - 0 0 -\n"
                    + "step 13 0 0 0 0 0 - 0 0 -\n";
            Ops.File f1 = Ops.File.parse(conf);
            Ops.File f2 = Ops.File.parse(f1.serialize());
            boolean round = f2.ops.size() == 2;
            if (round) {
                for (int q = 0; q < 2; q++) {
                    Ops.Op a = f1.ops.get(q), b = f2.find(a.name);
                    round = b != null && a.steps.size() == b.steps.size();
                    if (!round) break;
                    for (int i = 0; i < a.steps.size(); i++) {
                        Ops.Step sa = a.steps.get(i), sb = b.steps.get(i);
                        if (sa.type != sb.type || sa.a1 != sb.a1 || sa.a2 != sb.a2 || sa.a3 != sb.a3
                                || sa.a4 != sb.a4 || sa.ms != sb.ms || sa.j1 != sb.j1 || sa.j2 != sb.j2
                                || !sa.ref.equals(sb.ref) || !sa.expr.equals(sb.expr)) {
                            round = false;
                            why.append("往返不一致@").append(a.name).append(i + 1).append(' ');
                            break;
                        }
                    }
                }
            }

            /* b) 正例：zb1（多条件 IF + 终止）/ zb2（多条件 while + 跳出 2 层）validate 过 */
            String v1 = Ops.validate(f1.find("zb1"), f1, f1.find("zb1"));
            String v2 = Ops.validate(f1.find("zb2"), f1, f1.find("zb2"));
            boolean pos = v1 == null && v2 == null;
            if (!pos) why.append("正例被拒:").append(v1).append('/').append(v2).append(' ');

            /* c) 负例：位置错 / 超限 / 取反域 / 终止域 / 层数超深 / 跳到 19 */
            boolean negPos = ctrlRej(ctrlValOf("zb9", "19 0 0 100 200 0 r1 0 0 -"), "子条件位置错误");
            boolean negPos2 = ctrlRej(ctrlValOf("zb9", "12 1 2000 0 0 0 - 0 0 -",
                    "19 0 0 1 2 0 r1 0 0 -", "13 0 0 0 0 0 - 0 0 -"), "子条件位置错误");
            boolean negCap = ctrlRej(ctrlValOf("zb9", "14 0 5 6 0 0 r9 0 0 -",
                    "19 0 0 1 2 0 r1 0 0 -", "19 0 0 1 3 0 r2 0 0 -", "19 0 0 1 4 0 r3 0 0 -",
                    "19 0 0 1 5 0 r4 0 0 -", "16 0 0 0 0 0 - 0 0 -"), "子条件超限");
            boolean negInv = ctrlRej(ctrlValOf("zb9", "14 0 5 6 2 0 r9 0 0 -",
                    "16 0 0 0 0 0 - 0 0 -"), "字段必须为 0/1");
            boolean negStop = ctrlRej(ctrlValOf("zb9", "20 2 0 0 0 0 - 0 0 -"), "字段必须为 0/1");
            boolean negDepth = ctrlRej(ctrlValOf("zb9", "12 0 2 0 0 0 - 0 0 -",
                    "17 2 0 0 0 0 - 0 0 -", "13 0 0 0 0 0 - 0 0 -"), "跳出层数非法");
            boolean negJt19 = ctrlRej(ctrlValOf("zb9", "14 0 5 6 0 0 r9 0 0 -",
                    "19 0 0 1 2 0 r1 0 0 -", "8 2 0 0 0 0 - 0 0 -", "16 0 0 0 0 0 - 0 0 -"), "跳转跨越块边界");
            boolean negCmp = ctrlRej(ctrlValOf("zb9", "14 4 -6 100 0 0 - 6 0 -",
                    "16 0 0 0 0 0 - 0 0 -"), "比较符非法");
            boolean negVar = ctrlRej(ctrlValOf("zb9", "14 4 5 100 0 0 - 0 0 -",
                    "16 0 0 0 0 0 - 0 0 -"), "变量编码非法");

            /* d) setClauseGroup：加 / 减 / 清空（保序：其余步对象身份不变）+ 超上限拒且未改动 */
            List<Ops.Step> sc = new ArrayList<Ops.Step>();
            Ops.Step gh = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
            Ops.Step body = bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0);
            Ops.Step gend = bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0);
            sc.add(gh); sc.add(body); sc.add(gend);
            List<Ops.Step> cl = new ArrayList<Ops.Step>();
            cl.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));
            cl.add(bstep(Ops.T_CLAUSE, Ops.J_OR, 0, 10, 20, 0, "r1", "", 0, 0));
            boolean g1 = Ops.setClauseGroup(sc, 0, gh, cl) && sc.size() == 5
                    && sc.get(1).type == Ops.T_CLAUSE && sc.get(2).type == Ops.T_CLAUSE
                    && sc.get(3) == body && sc.get(4) == gend;
            List<Ops.Step> cl3 = new ArrayList<Ops.Step>();
            cl3.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));
            boolean g2 = Ops.setClauseGroup(sc, 0, gh, cl3) && sc.size() == 4
                    && sc.get(1).type == Ops.T_CLAUSE && sc.get(2) == body;
            List<Ops.Step> cl0 = new ArrayList<Ops.Step>();
            boolean g3 = Ops.setClauseGroup(sc, 0, gh, cl0) && sc.size() == 3 && sc.get(1) == body;
            List<Ops.Step> cl5 = new ArrayList<Ops.Step>();
            for (int k = 0; k < 4; k++) cl5.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));
            boolean g4 = !Ops.setClauseGroup(sc, 0, gh, cl5) && sc.size() == 3;
            boolean groups = g1 && g2 && g3 && g4;
            if (!groups) why.append("组替换=").append(g1).append('/').append(g2).append('/')
                    .append(g3).append('/').append(g4).append(' ');

            /* e) 摘要与导航助手 */
            boolean summ = bstep(Ops.T_CLAUSE, Ops.J_OR, 1, 0, 0, 0, "s1", "", 0, 1).clauseSummary()
                        .equals("或 · 非 开关判断 s1")
                    && bstep(Ops.T_STOP, 0, 0, 0, 0, 0, "", "", 0, 0).summary().equals("终止 · 完成收工")
                    && bstep(Ops.T_STOP, 1, 0, 0, 0, 0, "", "", 0, 0).summary().equals("终止 · 中止退出")
                    && bstep(Ops.T_CONT, 2, 0, 0, 0, 0, "", "", 0, 0).summary().equals("继续循环（2 层）")
                    && Ops.clauseTail(sc, 0) == 0 && Ops.clauseCount(sc, 0) == 1
                    && Ops.canClause(bstep(Ops.T_LOOP, 3, 0, 0, 0, 0, "", "", 0, 0))
                    && !Ops.canClause(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));
            /* f) v6C：变量比较摘要 + 槽名显示 + 找色多点修复 */
            boolean v4 = bstep(Ops.T_CLAUSE, Ops.J_AND, 4, -6, 100, 0, "", "", Ops.CMP_GT, 0).clauseBody()
                        .equals("变量 fx > 100")
                    && bstep(Ops.T_CLAUSE, Ops.J_OR, 3, 1, 0, 0, "pts1", "", 0, 0).clauseBody()
                        .equals("找色 多点 pts1");
            String[] vsl = new String[Ops.VAR_N];
            vsl[6] = "dx";
            boolean v4b = Ops.Step.varLabel(-16, vsl).equals("dx")
                    && Ops.Step.varLabel(-6, null).equals("fx")
                    && Ops.Step.varLabel(-25, vsl).equals("变量16")
                    && Ops.Step.varLabel(-8, vsl).equals("退役槽");

            boolean all = round && pos && negPos && negPos2 && negCap && negInv && negStop && negDepth
                    && negJt19 && negCmp && negVar && groups && summ && v4 && v4b && why.length() == 0;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "控制流 v6B：往返=" + round + " 正例=" + pos
                    + " 位置/超限=" + (negPos && negPos2 && negCap) + " 取反域=" + negInv
                    + " 终止域=" + negStop + " 层数=" + negDepth + " 跳结构行=" + negJt19
                    + " 比较域=" + (negCmp && negVar) + " 变量摘要=" + (v4 && v4b)
                    + " 组替换=" + groups + " 摘要=" + summ);
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 控制流 v6B 异常: " + t);
        }
    }

    /** 条件块分支插入（v12.2）：块导航定位（成立 / 否则 / 循环体）+ 插入配套 + 原步骤保序；覆盖嵌套 / 无否则 / 上限。 */
    private static void branchInsChecks(List<String> out, File logFile) {
        try {
            StringBuilder why = new StringBuilder();

            /* a) 导航定位：与 OpsPage 插入点计算同源（Ops.pairTail / ifTrueTail / blockHeadOf） */
            List<Ops.Step> na = new ArrayList<Ops.Step>();
            na.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));       // 0 如果（有否则）
            na.add(bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0));   // 1 成立分支步
            na.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));       // 2 否则
            na.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));   // 3 否则分支步
            na.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));      // 4 条件块结束
            boolean nav1 = Ops.ifTrueTail(na, 0) == 2 && Ops.pairTail(na, 0) == 4 && Ops.blockHeadOf(na, 2) == 0;
            List<Ops.Step> nb = new ArrayList<Ops.Step>();
            nb.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));
            nb.add(bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0));
            nb.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));
            boolean nav2 = Ops.ifTrueTail(nb, 0) == 2 && Ops.pairTail(nb, 0) == 2;   // 无否则：落点 = 配对尾
            List<Ops.Step> nc = new ArrayList<Ops.Step>();                           // 嵌套：真分支含循环 + 跳出
            nc.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));
            nc.add(bstep(Ops.T_LOOP, 0, 2, 0, 0, 0, "", "", 0, 0));
            nc.add(bstep(Ops.T_BREAK, 0, 0, 0, 0, 0, "", "", 0, 0));
            nc.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));
            nc.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));
            nc.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));
            boolean nav3 = Ops.ifTrueTail(nc, 0) == 4 && Ops.pairTail(nc, 0) == 5 && Ops.pairTail(nc, 1) == 3
                    && Ops.blockHeadOf(nc, 4) == 0;
            List<Ops.Step> nd = new ArrayList<Ops.Step>();                           // 嵌套条件块：内外否则不串
            nd.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));
            nd.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));
            nd.add(bstep(Ops.T_TAP, 700, 900, 0, 0, 50, "", "", 0, 0));
            nd.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));
            nd.add(bstep(Ops.T_TAP, 710, 910, 0, 0, 50, "", "", 0, 0));
            nd.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));
            nd.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));
            nd.add(bstep(Ops.T_TAP, 730, 930, 0, 0, 50, "", "", 0, 0));
            nd.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));
            boolean nav4 = Ops.ifTrueTail(nd, 0) == 6 && Ops.ifTrueTail(nd, 1) == 3
                    && Ops.blockHeadOf(nd, 3) == 1 && Ops.blockHeadOf(nd, 6) == 0 && Ops.pairTail(nd, 0) == 8;
            boolean nav5 = Ops.blockHeadOf(na, 0) == -1;                             // 顶层
            boolean navOk = nav1 && nav2 && nav3 && nav4 && nav5;
            if (!navOk) why.append("导航 ").append(nav1).append('/').append(nav2).append('/')
                    .append(nav3).append('/').append(nav4).append('/').append(nav5).append(' ');

            /* b) 成立分支插入（否则之前）：types / 保序 / validate 三查 */
            Ops.Step bIf = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
            Ops.Step bTa = bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0);
            Ops.Step bEl = bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step bTb = bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0);
            Ops.Step bEn = bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step bWt = bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0);
            List<Ops.Step> B1 = new ArrayList<Ops.Step>();
            B1.add(bIf); B1.add(bTa); B1.add(bEl); B1.add(bTb); B1.add(bEn);
            boolean ins1 = Ops.insertStep(B1, Ops.ifTrueTail(B1, 0), bWt);
            boolean ok1 = ins1 && seqTypes(B1).equals("14,1,3,15,1,16")
                    && keptOrder(B1, new Ops.Step[]{bIf, bTa, bEl, bTb, bEn})
                    && stValOf(B1) == null;
            if (!ok1) why.append("成立 ins=").append(ins1).append(" seq=").append(seqTypes(B1))
                    .append(" val=").append(stValOf(B1)).append(' ');

            /* c) 否则分支插入（配对尾之前） */
            Ops.Step cIf = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
            Ops.Step cTa = bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0);
            Ops.Step cEl = bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step cTb = bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0);
            Ops.Step cEn = bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step cWt = bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0);
            List<Ops.Step> B2 = new ArrayList<Ops.Step>();
            B2.add(cIf); B2.add(cTa); B2.add(cEl); B2.add(cTb); B2.add(cEn);
            int at2 = Ops.pairTail(B2, Ops.blockHeadOf(B2, 2));
            boolean ins2 = Ops.insertStep(B2, at2, cWt);
            boolean ok2 = ins2 && seqTypes(B2).equals("14,1,15,1,3,16")
                    && keptOrder(B2, new Ops.Step[]{cIf, cTa, cEl, cTb, cEn})
                    && stValOf(B2) == null;
            if (!ok2) why.append("否则 ins=").append(ins2).append(" seq=").append(seqTypes(B2))
                    .append(" val=").append(stValOf(B2)).append(' ');

            /* d) 无否则：成立分支落点 = 配对尾之前 */
            Ops.Step dIf = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
            Ops.Step dTa = bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0);
            Ops.Step dEn = bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step dWt = bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0);
            List<Ops.Step> B3 = new ArrayList<Ops.Step>();
            B3.add(dIf); B3.add(dTa); B3.add(dEn);
            boolean ins3 = Ops.insertStep(B3, Ops.ifTrueTail(B3, 0), dWt);
            boolean ok3 = ins3 && seqTypes(B3).equals("14,1,3,16") && stValOf(B3) == null;
            if (!ok3) why.append("无否则 ins=").append(ins3).append(" seq=").append(seqTypes(B3))
                    .append(" val=").append(stValOf(B3)).append(' ');

            /* e) 嵌套块插入：成立分支内插「条件块」（含配套）后再往内层成立分支插步 */
            Ops.Step eIf = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
            Ops.Step eTa = bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0);
            Ops.Step eEl = bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step eTb = bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0);
            Ops.Step eEn = bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0);
            Ops.Step eIn = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
            Ops.Step eWt = bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0);
            List<Ops.Step> B4 = new ArrayList<Ops.Step>();
            B4.add(eIf); B4.add(eTa); B4.add(eEl); B4.add(eTb); B4.add(eEn);
            boolean ins4a = Ops.insertStep(B4, Ops.ifTrueTail(B4, 0), eIn);
            boolean ins4b = ins4a && Ops.insertStep(B4, Ops.ifTrueTail(B4, 2), eWt);
            boolean ok4 = ins4b && seqTypes(B4).equals("14,1,14,3,15,16,15,1,16")
                    && keptOrder(B4, new Ops.Step[]{eIf, eTa, eEl, eTb, eEn})
                    && stValOf(B4) == null;
            if (!ok4) why.append("嵌套 seq=").append(seqTypes(B4)).append(" val=").append(stValOf(B4)).append(' ');

            /* f) 上限：凑满 MAX 后单步与结构块都拒插 */
            List<Ops.Step> B5 = new ArrayList<Ops.Step>();
            for (int i = 0; i < Ops.MAX_STEPS - 1; i++) B5.add(bstep(Ops.T_TAP, 1, 1, 0, 0, 50, "", "", 0, 0));
            boolean cap1 = Ops.insertStep(B5, 0, bstep(Ops.T_WAIT, 0, 0, 0, 0, 10, "", "", 0, 0))
                    && B5.size() == Ops.MAX_STEPS;
            boolean cap2 = !Ops.insertStep(B5, 0, bstep(Ops.T_WAIT, 0, 0, 0, 0, 10, "", "", 0, 0));
            boolean cap3 = !Ops.insertStep(B5, 0, bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));
            boolean capOk = cap1 && cap2 && cap3;
            if (!capOk) why.append("上限 ").append(cap1).append('/').append(cap2).append('/').append(cap3).append(' ');

            boolean all = navOk && ok1 && ok2 && ok3 && ok4 && capOk && why.length() == 0;
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ") + "块分支插入：定位=" + navOk
                    + " 成立=" + ok1 + " 否则=" + ok2 + " 无否则=" + ok3 + " 嵌套=" + ok4 + " 上限=" + capOk
                    + (why.length() > 0 ? " why=" + why : ""));
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 块分支插入异常: " + t);
        }
    }

    // ---------------------------------------------------------------- 复制与收起（v31）

    /** 步骤复制核（纯逻辑，v31）：深拷贝独立 / 落点过滤（子条件位排除）/ 复数粘贴互不影响 / 上限 / 预检 / 引用原样 / 连选归一。 */
    private static void copyFoldChecks(List<String> out, File logFile) {
        try {
            StringBuilder why = new StringBuilder();
            // 夹具：TAP / IF+CLAUSE / TAP / ENDIF / WAIT
            List<Ops.Step> src = new ArrayList<Ops.Step>();
            src.add(bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0));            // 0
            src.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));               // 1
            src.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));   // 2
            src.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));           // 3
            src.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));              // 4
            src.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 120, "", "", 0, 0));             // 5
            StepClip.reset();
            StepClip.copy(src, 1, 4, "src", Ops.varSlotsOf(src));                   // 整块（含子条件与块尾）
            boolean deepOk = StepClip.size() == 4;
            src.get(3).a1 = 999;                                                    // 改源不串味
            deepOk = deepOk && StepClip.steps.get(2).a1 == 300;
            // 粘贴两次 → 两份副本互为独立对象（改一份不影响另一份/剪贴板）
            List<Ops.Step> t2 = new ArrayList<Ops.Step>();
            t2.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 10, "", "", 0, 0));
            String pe1 = StepClip.paste(t2, 1);
            String pe2 = StepClip.paste(t2, 0);
            t2.get(0).a2 = 777;
            boolean indepOk = pe1 == null && pe2 == null && t2.size() == 9
                    && t2.get(5).a2 == 5 && t2.get(5) != t2.get(0)
                    && StepClip.steps.get(0).a2 == 5;
            // 落点过滤：单步单元插在「块头与其子条件之间」= 非法（子条件须紧跟块头），其余 6 点位合法
            List<Ops.Step> u1 = new ArrayList<Ops.Step>();
            u1.add(bstep(Ops.T_TAP, 1, 2, 0, 0, 50, "", "", 0, 0));
            List<Integer> pos = StepClip.legalPositions(src, u1);
            boolean posOk = pos.size() == 6 && !pos.contains(Integer.valueOf(2))
                    && pos.contains(Integer.valueOf(0)) && pos.contains(Integer.valueOf(6));
            // 预检：目标槽 0 = z vs 单元 calc e1(槽 0) + 用 -10/-11 → 有提醒；纯字面单元 → 无
            List<Ops.Step> pt = new ArrayList<Ops.Step>();
            pt.add(bstep(Ops.T_CALC, 0, 0, 0, 0, 0, "z", "1+1", 0, 0));
            List<Ops.Step> u3 = new ArrayList<Ops.Step>();
            u3.add(bstep(Ops.T_CALC, 0, 0, 0, 0, 0, "e1", "1+1", 0, 0));
            u3.add(bstep(Ops.T_SWIPE, -10, -11, 100, 200, 100, "", "", 0, 0));
            String n1 = StepClip.preflightNotice(pt, Ops.varSlotsOf(pt), u3, new String[Ops.VAR_N]);
            String n0 = StepClip.preflightNotice(new ArrayList<Ops.Step>(), new String[Ops.VAR_N], u1, null);
            boolean preOk = n1 != null && n1.contains("变量引用") && n0 == null;
            // 引用原样：粘贴 u3 到空目标 → 负编码未改写
            StepClip.copy(u3, 0, 1, "u3", null);
            List<Ops.Step> t3 = new ArrayList<Ops.Step>();
            String pe3 = StepClip.paste(t3, 0);
            boolean verbatim = pe3 == null && t3.size() == 2 && t3.get(1).a1 == -10 && t3.get(1).a2 == -11;
            // 上限：511 + 2 → 无落点且粘贴被拒
            List<Ops.Step> big = new ArrayList<Ops.Step>();
            for (int i = 0; i < Ops.MAX_STEPS - 1; i++) big.add(bstep(Ops.T_TAP, 1, 1, 0, 0, 50, "", "", 0, 0));
            StepClip.copy(big, 0, 1, "big", null);
            String capErr = StepClip.paste(big, 0);
            boolean capOk = StepClip.legalPositions(big, StepClip.steps).isEmpty()
                    && capErr != null && capErr.contains("上限");
            // 连选归一：LOOP{TAP, IF+CLAUSE, TAP, ELSE, TAP, ENDIF} ENDLOOP, WAIT
            List<Ops.Step> nb = new ArrayList<Ops.Step>();
            nb.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));                 // 0
            nb.add(bstep(Ops.T_TAP, 1, 1, 0, 0, 50, "", "", 0, 0));                 // 1
            nb.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));                 // 2
            nb.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));     // 3
            nb.add(bstep(Ops.T_TAP, 2, 2, 0, 0, 50, "", "", 0, 0));                 // 4
            nb.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));                 // 5
            nb.add(bstep(Ops.T_TAP, 3, 3, 0, 0, 50, "", "", 0, 0));                 // 6
            nb.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));                // 7
            nb.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));              // 8
            nb.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 120, "", "", 0, 0));               // 9
            boolean nOk = rEq(StepClip.normalizeRange(nb, 1, 1), 1, 1)              // 普通步不扩张
                    && rEq(StepClip.normalizeRange(nb, 3, 3), 2, 7)                 // 子条件 → 整块
                    && rEq(StepClip.normalizeRange(nb, 0, 1), 0, 8)                 // 循环头 → 到循环尾
                    && rEq(StepClip.normalizeRange(nb, 5, 5), 2, 7)                 // 否则 → 整块
                    && rEq(StepClip.normalizeRange(nb, 6, 7), 2, 7)                 // 块内尾段 → 整块
                    && rEq(StepClip.normalizeRange(nb, 8, 9), 0, 9);                // 循环尾 → 含头
            StepClip.reset();
            boolean all = deepOk && indepOk && posOk && capOk && preOk && verbatim && nOk;
            if (!all) why.append("deep=").append(deepOk).append(" indep=").append(indepOk)
                    .append(" pos=").append(posOk).append(" cap=").append(capOk)
                    .append(" pre=").append(preOk).append(" verbatim=").append(verbatim).append(" norm=").append(nOk);
            rec(out, logFile, (all ? "[OK] " : "[FAIL] ")
                    + "步骤复制核：深拷贝=" + deepOk + " 复数粘贴独立=" + indepOk + " 落点过滤=" + posOk
                    + " 上限=" + capOk + " 预检=" + preOk + " 引用原样=" + verbatim + " 连选归一=" + nOk
                    + (all ? "" : " why=" + why));
        } catch (Throwable t) {
            rec(out, logFile, "[FAIL] 步骤复制核异常: " + t);
        }
    }

    /** 归一区间相等断言（s,e 含端点；null = 失败）。 */
    private static boolean rEq(int[] r, int s, int e) {
        return r != null && r[0] == s && r[1] == e;
    }

    /** 步骤类型序号串（块分支插入用例）："14,1,3,…"。 */
    private static String seqTypes(List<Ops.Step> l) {
        StringBuilder sb = new StringBuilder();
        for (Ops.Step s : l) {
            if (sb.length() > 0) sb.append(',');
            sb.append(s.type);
        }
        return sb.toString();
    }

    /** 原有步骤对象保序检查（indexOf 严格递增 = 未被改写/换位）。 */
    private static boolean keptOrder(List<Ops.Step> l, Ops.Step[] orig) {
        int prev = -1;
        for (Ops.Step s : orig) {
            int i = l.indexOf(s);
            if (i <= prev) return false;
            prev = i;
        }
        return true;
    }

    /** 步骤序列 → conf 文本 → parse → validate；返回 null = 通过（块分支插入用例）。 */
    private static String stValOf(List<Ops.Step> steps) {
        StringBuilder sb = new StringBuilder("#vtouch-ops v5\nop zq1 gate - autooff 0\n");
        for (Ops.Step s : steps) {
            sb.append("step ").append(s.type).append(' ').append(s.a1).append(' ').append(s.a2).append(' ')
              .append(s.a3).append(' ').append(s.a4).append(' ').append(s.ms).append(' ')
              .append(s.ref.isEmpty() ? "-" : s.ref).append(' ').append(s.j1).append(' ').append(s.j2).append(' ')
              .append(s.expr.isEmpty() ? "-" : s.expr).append('\n');
        }
        Ops.File f = Ops.File.parse(sb.toString());
        Ops.Op o = f.find("zq1");
        return o == null ? "解析失败" : Ops.validate(o, f, o);
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
                StepEdit se = StepEdit.build(ctx, sv, idsList, idsList, null, null, new String[Ops.VAR_N], null, null);
                renderWrap(ctx, se.root, "edit_step.png", Ui.BG);
                // 3.0b) 计算步 / 找图步编辑页预览（v6 新编辑器）
                Ops.Step sc = new Ops.Step();
                sc.type = Ops.T_CALC; sc.a1 = 0; sc.ref = "dx"; sc.expr = "atan2(tuy-tdy, tux-tdx)";
                StepEdit se2 = StepEdit.build(ctx, sc, idsList, idsList, null, null, new String[Ops.VAR_N], null, null);
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
                StepEdit se3 = StepEdit.build(ctx, sf, idsList, idsList, null, null, new String[Ops.VAR_N], dv, null);
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
                // 3.2) 移动步骤对话框内容预览（真文案；v26：块头=整块移动）
                java.util.List<String> mvItems = new java.util.ArrayList<String>();
                mvItems.add("移到第 1 位（开头）");
                mvItems.add("移到第 2 位");
                mvItems.add("移到第 4 位");
                mvItems.add("移到末尾（第 5 位）");
                Ui.Dlg md = Forms.pickDialogBuild(ctx, "移动「3. 条件块」整块（5 步）到…", mvItems, -1, null);
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
                android.view.View sfv = StepFlow.build(ctx, fsteps, null, null, null);
                renderWrap(ctx, sfv, "step_flow.png", Ui.BG);
                /* 控制流块树（v12）：嵌套循环 + 条件块 + 否则（形态 C 色带块） */
                try {
                    List<Ops.Step> bs = new ArrayList<Ops.Step>();
                    bs.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));                 // 0 循环
                    bs.add(bstep(Ops.T_COND_REGION, 100, 200, 1, 2, 0, "r9", "", 0, 0));   // 1 区域判断
                    bs.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));                // 2 条件块
                    bs.add(bstep(Ops.T_CLAUSE, Ops.J_OR, 1, 0, 0, 0, "s1", "", 0, 0));     // 3 子条件（或·开关判断 s1）
                    bs.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));            // 4 成立分支
                    bs.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));                // 5 否则
                    bs.add(bstep(Ops.T_TAP, 500, 600, 0, 0, 50, "", "", 0, 0));            // 6 否则分支
                    bs.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));               // 7 条件块结束
                    bs.add(bstep(Ops.T_LOOP, 1, 1800000, 0, 0, 0, "", "", 0, 0));
                    bs.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 120, "", "", 0, 0));
                    bs.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));
                    bs.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));
                    int[] bdep = StepFlow.depthsOf(bs);
                    boolean depOk = bdep[2] == bdep[5] && bdep[3] == bdep[2]      // v12.2：否则=如果同级；v6B：子条件=如果同级
                            && bdep[4] == bdep[2] + 1                             // 块内 +1
                            && bdep[7] == bdep[2] && bdep[0] == 0;                // 块尾=块头；顶层
                    android.view.View flowV = StepFlow.build(ctx, bs, null, null, null);
                    boolean flowTxt = containsText(flowV, "或 · 开关判断 s1");
                    renderWrap(ctx, flowV, "edit_ops_flow.png", Ui.BG);
                    /* v12.4 块尾核：块尾行与块头同级（用户口径「条件块结束应当和条件块在一个缩进等级内」）——
                       「条件块结束」左缘 == 「条件块」左缘（含 chip 行 = 14+26+14+10+3+6dp +8dp 布局偏移=81dp）；
                       顶层「循环结束」 == 「循环」（无 chip = 14+26+10dp +8dp=58dp）。 */
                    TextView tbHead = findText(flowV, "条件块");
                    TextView tbTail = findText(flowV, "条件块结束");
                    TextView lpHead = findText(flowV, "循环");
                    TextView lpTail = findTextLast(flowV, "循环结束");
                    boolean tailOk = tbHead != null && tbTail != null && lpHead != null && lpTail != null
                            && Math.abs(absLeft(tbHead) - absLeft(tbTail)) <= 3
                            && Math.abs(absLeft(tbTail) - Ui.dp(ctx, 81)) <= 3
                            && Math.abs(absLeft(lpHead) - absLeft(lpTail)) <= 3
                            && Math.abs(absLeft(lpTail) - Ui.dp(ctx, 58)) <= 3;
                    /* v12.3：块头 ▼/▸ 收起核（收起条件块 idx2 → 隐藏 3 行；渲染件 fold） */
                    java.util.Set<Ops.Step> foldSet = java.util.Collections.newSetFromMap(
                            new java.util.IdentityHashMap<Ops.Step, Boolean>());
                    foldSet.add(bs.get(2));
                    android.view.View foldV = StepFlow.build(ctx, bs, null, foldSet, null);
                    boolean foldOk = containsText(foldV, "已收起 4 步") && containsText(foldV, "条件块结束")
                            && !containsText(foldV, "300,400") && !containsText(foldV, "500,600")
                            && !containsText(foldV, "开关判断 s1")
                            && containsText(foldV, "▸") && containsText(foldV, "▼");
                    renderWrap(ctx, foldV, "edit_ops_flow_fold.png", Ui.BG);
                    rec(out, logFile, (depOk && foldOk && flowTxt && tailOk ? "[OK] " : "[FAIL] ")
                            + "控制流块树预览：edit_ops_flow.png + 收起件 edit_ops_flow_fold.png（否则/子条件同级=" + depOk
                            + " 收起核=" + foldOk + " 子条件行=" + flowTxt + " 块尾核=" + tailOk + "）");
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] 控制流块树预览异常: " + t);
                }
                /* 操作块卡片（v12.2）：条件块 成立 [＋] / 「否则」分隔行 [＋]（离屏直驱 OpsPage 编辑态） */
                try {
                    List<Ops.Step> cbs = new ArrayList<Ops.Step>();
                    cbs.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));                   // 1 循环
                    cbs.add(bstep(Ops.T_TAP, 154, 1978, 0, 0, 50, "", "", 0, 0));             // 2 点按
                    cbs.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "c1", "", 0, 0));                  // 3 条件块
                    cbs.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));      // 4 子条件（且·开关判断 s1）
                    cbs.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));              // 5 成立分支
                    cbs.add(bstep(Ops.T_LOOP, 0, 2, 0, 0, 0, "", "", 0, 0));                  // 6 内层循环（第 3 层壳 · v6C 评审 F1）
                    cbs.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));                // 7 内层循环体
                    cbs.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));               // 8 内层循环结束
                    cbs.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));                  // 9 否则
                    cbs.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));                // 10 否则分支
                    cbs.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));                 // 11 条件块结束
                    cbs.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));               // 12 循环结束
                    Ops.Op cbOp = new Ops.Op();
                    cbOp.name = "挂机";
                    cbOp.gate = "-";
                    for (Ops.Step st : cbs) cbOp.steps.add(st);
                    OpsPage cbPg = new OpsPage(ctx);
                    View cbRoot = cbPg.build();
                    cbPg.openEditorForTest(cbOp);
                    android.widget.LinearLayout cbWrap = Ui.col(ctx);
                    cbWrap.setBackgroundColor(Ui.BG);
                    cbWrap.addView(cbRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    renderView(ctx, cbWrap, "edit_ops_blocks.png");
                    List<String> cbViol = new ArrayList<String>();
                    assertOverflow(cbWrap, "块卡片", cbViol, ctx);
                    boolean cbTxt = containsText(cbWrap, "循环") && containsText(cbWrap, "条件块")
                            && containsText(cbWrap, "否则") && containsText(cbWrap, "＋ 添加步骤")
                            && containsText(cbWrap, "且 · 开关判断 s1");
                    /* v12.3/v6B/v6C 代码式缩进核（14dp/级，±3px）：1.循环=20dp / 3.条件块=4.子条件=9.否则=34dp /
                       5.点按=10.等待=48dp / 6.内层循环=48dp（第 3 层壳）/ 7.内层循环体=62dp（评审 F1 深核） */
                    TextView ci0 = findText(cbWrap, "1. 循环");
                    TextView ci1 = findText(cbWrap, "9. 否则");
                    TextView ci2 = findText(cbWrap, "10. 等待");
                    TextView ci3 = findText(cbWrap, "3. 条件块");
                    TextView ci4 = findText(cbWrap, "4. 且 · 开关判断 s1");
                    TextView ci5 = findText(cbWrap, "5. 点按");
                    TextView ci6 = findText(cbWrap, "6. 循环");
                    TextView ci7 = findText(cbWrap, "7. 等待");
                    boolean indOk = ci0 != null && ci1 != null && ci2 != null && ci3 != null && ci4 != null
                            && ci5 != null && ci6 != null && ci7 != null
                            && Math.abs(absLeft(ci0) - Ui.dp(ctx, 20)) <= 3      // 顶层块头
                            && Math.abs(absLeft(ci3) - Ui.dp(ctx, 34)) <= 3      // 条件块头（内层）
                            && Math.abs(absLeft(ci4) - Ui.dp(ctx, 34)) <= 3      // v6B：子条件行 = 块头同级
                            && Math.abs(absLeft(ci1) - Ui.dp(ctx, 34)) <= 3      // 否则 = 块头同级
                            && Math.abs(absLeft(ci5) - Ui.dp(ctx, 48)) <= 3      // 块内 +1
                            && Math.abs(absLeft(ci2) - Ui.dp(ctx, 48)) <= 3      // 否则分支体 +1
                            && Math.abs(absLeft(ci6) - Ui.dp(ctx, 48)) <= 3      // 第 3 层壳：内层循环头（评审 F1 修后）
                            && Math.abs(absLeft(ci7) - Ui.dp(ctx, 62)) <= 3;     // 第 3 层壳体 +1（评审 F1 修后）
                    /* v6B/v6C：嵌套色带右移核（壳 marginLeft 相对父壳 = 0/14/14dp —— 评审 F1 修为常量 14 防累积） */
                    boolean marginOk = cbPg.testShellMargins.size() == 3
                            && cbPg.testShellMargins.get(0) == 0
                            && cbPg.testShellMargins.get(1) == Ui.dp(ctx, 14)
                            && cbPg.testShellMargins.get(2) == Ui.dp(ctx, 14);
                    /* v12.4 块尾核：卡片侧块尾行=块头同级（「条件块结束」左缘 == 「3. 条件块」左缘 == 34dp；回归护栏） */
                    TextView cbTail = findText(cbWrap, "条件块结束");
                    boolean tailOk = ci3 != null && cbTail != null
                            && Math.abs(absLeft(ci3) - absLeft(cbTail)) <= 3
                            && Math.abs(absLeft(cbTail) - Ui.dp(ctx, 34)) <= 3;
                    rec(out, logFile, (cbViol.isEmpty() && cbTxt && indOk && marginOk && tailOk ? "[OK] " : "[FAIL] ")
                            + "操作块卡片预览：edit_ops_blocks.png（成立 [＋] / 否则分隔行 [＋] / 子条件行） 关键文本=" + cbTxt
                            + " 缩进核=" + indOk + " 色带步进=" + marginOk + " 块尾核=" + tailOk
                            + (cbViol.isEmpty() ? " · 溢出核 无" : " · 溢出 " + cbViol.get(0)));
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] 操作块卡片预览异常: " + t);
                }
                /* v31：卡片视图收起 / 连选 / 粘贴弹窗（渲染件 edit_ops_card_fold / edit_ops_sel / paste_dialog） */
                try {
                    List<Ops.Step> fs = new ArrayList<Ops.Step>();
                    fs.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));                 // 0 循环×3
                    fs.add(bstep(Ops.T_TAP, 154, 1978, 0, 0, 50, "", "", 0, 0));            // 1
                    fs.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "c1", "", 0, 0));                 // 2 条件块
                    fs.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));     // 3 子条件
                    fs.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));             // 4
                    fs.add(bstep(Ops.T_LOOP, 0, 2, 0, 0, 0, "", "", 0, 0));                 // 5 内层循环
                    fs.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));               // 6
                    fs.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));              // 7
                    fs.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));                 // 8 否则
                    fs.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));               // 9
                    fs.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));                // 10
                    fs.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));              // 11
                    fs.add(bstep(Ops.T_TAP, 5, 6, 0, 0, 50, "", "", 0, 0));                 // 12
                    OpsPage fPg = new OpsPage(ctx);
                    View fRoot = fPg.build();
                    Ops.Op fOp = new Ops.Op();
                    fOp.name = "折叠";
                    fOp.gate = "-";
                    for (Ops.Step st : fs) fOp.steps.add(st);
                    fPg.openEditorForTest(fOp);
                    fPg.toggleFoldAt(2);                       // 收起条件块（配对尾 10 → 已收起 7 步）
                    android.widget.LinearLayout fWrap = Ui.col(ctx);
                    fWrap.setBackgroundColor(Ui.BG);
                    fWrap.addView(fRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    renderView(ctx, fWrap, "edit_ops_card_fold.png");
                    List<String> fViol = new ArrayList<String>();
                    assertOverflow(fWrap, "卡片收起", fViol, ctx);
                    boolean fTxt = containsText(fWrap, "已收起 7 步") && containsText(fWrap, "条件块结束")
                            && containsText(fWrap, "▸") && containsText(fWrap, "▼")
                            && !containsText(fWrap, "且 · 开关判断 s1") && !containsText(fWrap, "300,400")
                            && !containsText(fWrap, "100ms");
                    int[] fRow = fPg.focusMetricsForTest(4);       // 隐藏行不在行表
                    int[] fRow2 = fPg.focusMetricsForTest(2);      // 块头在行表
                    boolean rowOk = fRow[0] == -1 && fRow2[0] >= 0;
                    /* 联动核：往收起块内插入 → 自动展开 + 定位到新行 */
                    String fIns = fPg.insertNewStep(4, bstep(Ops.T_WAIT, 0, 0, 0, 0, 250, "", "", 0, 0), "");
                    boolean unOk = fIns == null && !containsText(fWrap, "已收起")
                            && containsText(fWrap, "且 · 开关判断 s1") && containsText(fWrap, "250ms")
                            && fPg.focusIdxRequest == 4;
                    /* 连选核（同形状新夹具）：长按≈beginSel + 点行≈extendSel；内层循环整块归一 */
                    List<Ops.Step> ss = new ArrayList<Ops.Step>();
                    ss.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));
                    ss.add(bstep(Ops.T_TAP, 154, 1978, 0, 0, 50, "", "", 0, 0));
                    ss.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "c1", "", 0, 0));
                    ss.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0));
                    ss.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));
                    ss.add(bstep(Ops.T_LOOP, 0, 2, 0, 0, 0, "", "", 0, 0));
                    ss.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));
                    ss.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));
                    ss.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));
                    ss.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));
                    ss.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));
                    ss.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));
                    ss.add(bstep(Ops.T_TAP, 5, 6, 0, 0, 50, "", "", 0, 0));
                    Ops.Op sOp = new Ops.Op();
                    sOp.name = "连选";
                    sOp.gate = "-";
                    for (Ops.Step st : ss) sOp.steps.add(st);
                    OpsPage sPg = new OpsPage(ctx);
                    View sRoot = sPg.build();
                    sPg.openEditorForTest(sOp);
                    sPg.beginSel(5);
                    sPg.extendSel(6);                              // → 归一 [5..7]（内层循环整块）
                    int[] selR = sPg.selRange();
                    android.widget.LinearLayout sWrap = Ui.col(ctx);
                    sWrap.setBackgroundColor(Ui.BG);
                    sWrap.addView(sRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    renderView(ctx, sWrap, "edit_ops_sel.png");
                    List<String> sViol = new ArrayList<String>();
                    assertOverflow(sWrap, "连选", sViol, ctx);
                    boolean selOk = selR != null && selR[0] == 5 && selR[1] == 7
                            && containsText(sWrap, "已选 3 步") && containsText(sWrap, "复制所选")
                            && sViol.isEmpty();
                    /* 展示优化：选中段默认落在折叠线下 → 滚动到选中段后重拍（首轮已保底） */
                    int[] fmSel = sPg.focusMetricsForTest(5);
                    if (fmSel[0] >= 0) {
                        sPg.listScrollToForTest(Math.max(0, fmSel[0] - Ui.dp(ctx, 80)));
                        renderView(ctx, sWrap, "edit_ops_sel.png");
                    }
                    /* 粘贴弹窗预览：单元=内层循环整块（3 步）；快捷项 末尾/开头 应居首 */
                    StepClip.copy(ss, 5, 7, "连选", Ops.varSlotsOf(ss));
                    boolean bld = sPg.buildPasteItems();
                    boolean labOk = bld && sPg.pasteLabels.size() >= 2
                            && sPg.pasteLabels.get(0).contains("粘贴到末尾")
                            && sPg.pasteLabels.get(1).contains("粘贴到第 1 位（开头）");
                    Ui.Dlg pd = Forms.pickDialogBuild(ctx, sPg.pasteTitle, sPg.pasteLabels, -1, null);
                    boolean dlgOk = labOk && containsText(pd.root, "粘贴到末尾")
                            && containsText(pd.root, "粘贴到第 1 位（开头）")
                            && containsText(pd.root, "粘贴「3 步」到…");
                    renderWrap(ctx, pd.root, "paste_dialog.png", 0xFF0C0E10);
                    /* 快捷项「原位置之后」：单步单元从第 2 位复制 → 第 3 位可粘 */
                    StepClip.copy(ss, 1, 1, "连选", Ops.varSlotsOf(ss));
                    sPg.buildPasteItems();
                    boolean srcOk = sPg.pasteLabels.contains("粘贴到原位置之后（第 3 位）");
                    StepClip.reset();
                    boolean all = fTxt && rowOk && unOk && selOk && dlgOk && srcOk && fViol.isEmpty();
                    rec(out, logFile, (all ? "[OK] " : "[FAIL] ")
                            + "复制与收起预览：edit_ops_card_fold.png + edit_ops_sel.png + paste_dialog.png"
                            + "（收起核=" + fTxt + " 行表核=" + rowOk + " 联动核=" + unOk
                            + " 连选核=" + selOk + " 弹窗核=" + dlgOk + " 原位置项=" + srcOk
                            + (fViol.isEmpty() ? " · 溢出核 无" : " · 溢出 " + fViol.get(0)) + "）");
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] 复制与收起预览异常: " + t);
                }
                /* 判定编辑器切换（v12.2）：条件块 Seg 切类型后子配置须重建（修复核） */
                try {
                    List<String> sids = new ArrayList<String>();
                    sids.add("c1");
                    Ops.Step ifs = bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0);
                    StepEdit seK = StepEdit.build(ctx, ifs, sids, sids, null, null, new String[Ops.VAR_N], null, null);
                    boolean k0 = containsText(seK.root, "判定点 x");
                    boolean k2 = seK.condTapForTest(2) && containsText(seK.root, "阈值（0..255）")
                            && !containsText(seK.root, "判定点 x");
                    boolean k1 = seK.condTapForTest(1) && containsText(seK.root, "开关型区域 id")
                            && !containsText(seK.root, "阈值（0..255）");
                    boolean k3 = seK.condTapForTest(3) && containsText(seK.root, "单点（颜色+容差）")
                            && !containsText(seK.root, "判定点 x");
                    boolean k4 = seK.condTapForTest(4) && containsText(seK.root, "参照值（整数）")
                            && containsText(seK.root, "变量（点「变量」选择") && !containsText(seK.root, "判定点 x");
                    boolean k0b = seK.condTapForTest(0) && containsText(seK.root, "判定点 x")
                            && !containsText(seK.root, "单点（颜色+容差）");
                    boolean kOk = k0 && k2 && k1 && k3 && k4 && k0b;
                    rec(out, logFile, (kOk ? "[OK] " : "[FAIL] ") + "判定切换核：初始=" + k0
                            + " 找图=" + k2 + " 开关=" + k1 + " 找色=" + k3 + " 变量=" + k4 + " 回区域=" + k0b);
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] 判定切换核异常: " + t);
                }
                /* v6B/v6C：块编辑器「条件列表」（渲染件 edit_ops_clause.png：子条件同页内联编辑器 + [＋]） */
                try {
                    java.util.List<Ops.Step> clHeadRows = new java.util.ArrayList<Ops.Step>();
                    clHeadRows.add(bstep(Ops.T_CLAUSE, Ops.J_OR, 1, 0, 0, 0, "s1", "", 0, 1));   // 或 · 非 开关判断 s1
                    clHeadRows.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 4, -6, 100, 0, "", "", Ops.CMP_GT, 0)); // v6C：且 · 变量 fx > 100
                    Ops.Step ifh = bstep(Ops.T_IF, 2, 8, 1, 1, 0, "tpl1", "", 0, 0);
                    StepEdit seC = StepEdit.build(ctx, ifh, idsList, idsList, idsList, idsList,
                            new String[Ops.VAR_N], null, clHeadRows);
                    renderWrap(ctx, seC.root, "edit_ops_clause.png", Ui.BG);
                    java.util.List<String> clViol = new ArrayList<String>();
                    assertOverflow(seC.root, "条件列表", clViol, ctx);
                    boolean clOk = containsText(seC.root, "加子条件") && containsText(seC.root, "取反（非）")
                            && containsText(seC.root, "子条件 1") && containsText(seC.root, "子条件 2")
                            && containsText(seC.root, "开关型区域 id") && containsText(seC.root, "参照值（整数）")
                            && containsText(seC.root, "≠")
                            && seC.clauseBoxCountForTest() == 2 && seC.clauseTapForTest(0, 4)
                            && clViol.isEmpty();
                    String clSeg = seC.segFitDetailForTest();                 // v32：判定行格宽体检（防 4 字标签裁尾字）
                    boolean clFit = clSeg.startsWith("适配");
                    rec(out, logFile, (clOk && clFit ? "[OK] " : "[FAIL] ") + "条件列表编辑器：edit_ops_clause.png（同页内联：判定类型/取反/参数 + 变量比较 + ＋ 加子条件） 判定行=" + clSeg
                            + (clViol.isEmpty() ? " · 溢出核 无" : " · 溢出 " + clViol.get(0)));
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] 条件列表编辑器异常: " + t);
                }
                /* 新建操作草稿（v21/22）：创建只查名 → 空态编辑页（渲染件）+ 空步 [完成] 拦 → 带步 [完成] 创建落回列表 + 自动落盘 */
                try {
                    OpsPage dPg = new OpsPage(ctx);
                    View dRoot = dPg.build();
                    dPg.openEditorForTest(new Ops.Op());          // 离屏直驱前置：补 file（空表）
                    String dErr0 = dPg.startNewDraft("新操作");    // 真实新建入口（只查名放行）
                    java.io.File autoF = new java.io.File(ctx.getExternalFilesDir(null), "vte-ops-auto.conf");
                    dPg.pathOverride = autoF.getAbsolutePath();   // v22：提交即落盘——重定向，防写真盘
                    Su.run("rm -f '" + autoF.getAbsolutePath() + "'");   /* 清旧，防上轮残留假阳性 */
                    android.widget.LinearLayout dWrap = Ui.col(ctx);
                    dWrap.setBackgroundColor(Ui.BG);
                    dWrap.addView(dRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    renderView(ctx, dWrap, "edit_ops_new.png");
                    List<String> dViol = new ArrayList<String>();
                    assertOverflow(dWrap, "新建草稿", dViol, ctx);
                    boolean dTxt = containsText(dWrap, "还没有步骤——点下方「＋ 添加步骤」开始")
                            && containsText(dWrap, "共 0 步") && containsText(dWrap, "＋ 添加步骤");
                    String dErr1 = dPg.commitEdit();              // 空步 [完成] → 仍拦
                    Ops.Op dOp2 = new Ops.Op();
                    dOp2.name = "新操作二";
                    dOp2.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));
                    dPg.openEditorForTest(dOp2);
                    String dErr2 = dPg.commitEdit();              // 带步 [完成] → isNew 创建 + 自动落盘（scratch）
                    boolean dAuto = false;                        // v22 自动落盘核：轮询 scratch 出现「op 新操作二」（异步写，≤5s）
                    for (int k = 0; k < 20 && !dAuto; k++) {
                        Thread.sleep(250);
                        String at = Su.read(autoF.getAbsolutePath());
                        dAuto = at != null && at.contains("op 新操作二");
                    }
                    String dErr3 = dPg.startNewDraft("新操作二");   // 重名 → 拒
                    boolean dFlow = dErr0 == null && dErr1 != null && dErr2 == null
                            && dErr3 != null && containsText(dRoot, "共 1 个操作")
                            && !containsText(dRoot, "保存");       // v22：列表 [保存] 已删
                    rec(out, logFile, (dTxt && dFlow && dAuto && dViol.isEmpty() ? "[OK] " : "[FAIL] ")
                            + "新建操作草稿：edit_ops_new.png（创建只查名 / 空步拦=" + (dErr1 != null)
                            + " / 带步创建落回列表） 自动落盘=" + dAuto + " 关键文本=" + dTxt + " 流程核=" + dFlow
                            + (dViol.isEmpty() ? " · 溢出核 无" : " · 溢出 " + dViol.get(0)));
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] 新建操作草稿预览异常: " + t);
                }
                /* v26 新批：① 会话保留（切页/刷新不打断编辑）② 添加定位 ③ 移动全覆盖（整块/组内/落点预检） */
                try {
                    // ③ 移动核：整块（循环+子条件）一起移动 / 越界拒 / 移出循环拒 / 子条件组内换序
                    OpsPage mPg = new OpsPage(ctx);
                    mPg.build();
                    Ops.Op mOp = new Ops.Op();
                    mOp.name = "移动";
                    mOp.steps.add(bstep(Ops.T_LOOP, 3, 0, 0, 0, 0, "s1", "", 0, 0));           // 0 循环·到条件（含子条件）
                    mOp.steps.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0)); // 1 子条件
                    mOp.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));           // 2 循环体
                    mOp.steps.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));          // 3 循环结束
                    mOp.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 120, "", "", 0, 0));           // 4 等待
                    mPg.openEditorForTest(mOp);
                    boolean mUnit = Ops.unitTail(mOp.steps, 0) == 3 && Ops.unitTail(mOp.steps, 4) == 4;
                    String mErr1 = mPg.moveUnitTo(0, 3);                    // 4 步整块 maxK=2 → 3 越界
                    boolean mRej1 = mErr1 != null && mOp.steps.get(0).type == Ops.T_LOOP;
                    String mErr2 = mPg.moveUnitTo(0, 2);                    // 整块（含子条件）移到末尾
                    boolean mBlk = mErr2 == null
                            && mOp.steps.get(0).type == Ops.T_WAIT && mOp.steps.get(4).type == Ops.T_ENDLOOP
                            && mOp.steps.get(1).type == Ops.T_LOOP && mOp.steps.get(2).type == Ops.T_CLAUSE
                            && mPg.focusIdxRequest == 1                          // 移动后定位到新位置
                            && Ops.structureErr(mOp.steps) == null;
                    Ops.Op bOp = new Ops.Op();
                    bOp.name = "移动B";
                    bOp.steps.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));             // 0
                    bOp.steps.add(bstep(Ops.T_BREAK, 1, 0, 0, 0, 0, "", "", 0, 0));            // 1 跳出（循环内）
                    bOp.steps.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));          // 2
                    bOp.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 100, "", "", 0, 0));           // 3
                    mPg.openEditorForTest(bOp);
                    String mErr3 = mPg.moveUnitTo(1, 3);                    // 跳出移到循环外 → 结构预检拒 + 回滚
                    boolean mRej2 = mErr3 != null && bOp.steps.get(1).type == Ops.T_BREAK;
                    Ops.Op gOp = new Ops.Op();
                    gOp.name = "移动G";
                    gOp.steps.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));             // 0 条件块
                    gOp.steps.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0)); // 1 子条件 A
                    gOp.steps.add(bstep(Ops.T_CLAUSE, Ops.J_OR, 0, 10, 20, 0, "r1", "", 0, 0)); // 2 子条件 B
                    gOp.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));           // 3 体
                    gOp.steps.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));            // 4
                    mPg.openEditorForTest(gOp);
                    String mErr4 = mPg.moveClauseTo(1, 2);                  // A ↔ B 组内换序
                    boolean mCl = mErr4 == null
                            && gOp.steps.get(1).a1 == Ops.J_OR && gOp.steps.get(2).a1 == Ops.J_AND
                            && mPg.focusIdxRequest == 2
                            && Ops.structureErr(gOp.steps) == null;
                    String mErr5 = mPg.moveClauseTo(2, 3);                  // 组内越界 → 拒
                    String mErr6 = mPg.moveClauseTo(1, 1);                  // 原位 → 拒
                    boolean mRej3 = mErr5 != null && mErr6 != null;
                    rec(out, logFile, (mUnit && mRej1 && mBlk && mRej2 && mCl && mRej3 ? "[OK] " : "[FAIL] ")
                            + "移动核 v26：单元尾=" + mUnit + " 越界拒=" + mRej1 + " 整块→末位（子条件随移）=" + mBlk
                            + " 跳出移出循环拒=" + mRej2 + " 子条件组内换序=" + mCl + " 组内越界/原位拒=" + mRej3);

                    /* v32：变量名显示核（框内显名不显编码）+ 新增块带子条件核（同页配的子条件随 [保存] 一起插入） */
                    try {
                        Forms.Num fn = new Forms.Num(ctx, "t", -6);
                        fn.enableVar(java.util.Arrays.asList("dx", "花费"), java.util.Arrays.asList(-10, -11));
                        boolean n1 = "fx".equals(fn.field.getText().toString()) && fn.value() == -6;
                        fn.setValue(-11);
                        boolean n2 = "花费".equals(fn.field.getText().toString()) && fn.value() == -11;
                        fn.setValue(-25);
                        boolean n3 = "-25".equals(fn.field.getText().toString());
                        String[] vsl = new String[Ops.VAR_N];
                        vsl[0] = "dx"; vsl[1] = "dy";
                        boolean n4 = bstep(Ops.T_TAP, -6, -7, 0, 0, 50, "", "", 0, 0).summary(vsl)
                                        .equals("点按 fx,fy  50ms")
                                && bstep(Ops.T_BOX, -6, -7, -10, -11, 3000, "", "", 0, 0).summary(vsl)
                                        .equals("框矩形 (fx,fy)–(dx,dy)（3 秒）")
                                && bstep(Ops.T_IF, 0, -6, -7, 0, 0, "r9", "", 0, 0).summary(vsl)
                                        .equals("如果 · 区域判断 r9 @fx,fy");
                        rec(out, logFile, (n1 && n2 && n3 && n4 ? "[OK] " : "[FAIL] ")
                                + "变量名显示核 v32：框内显名=" + n1 + " 中文名=" + n2 + " 无槽名保持编码=" + n3
                                + " 摘要显名=" + n4);

                        OpsPage xPg = new OpsPage(ctx);
                        View xRoot = xPg.build();
                        Ops.Op xOp = new Ops.Op();
                        xOp.name = "新块";
                        xOp.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));
                        xPg.openEditorForTest(xOp);
                        xPg.startNewStepForTest(Ops.T_IF, -1, "");         // 新增条件块（同页配子条件）
                        StepEdit xEdit = xPg.stepEditorForTest();
                        boolean xAdd = xEdit != null && xEdit.clauseAddForTest() && xEdit.clauseBoxCountForTest() == 1;
                        boolean xTap = xEdit != null && xEdit.clauseTapForTest(0, 4) && xEdit.condTapForTest(4);
                        String xErr = xPg.saveStepEdit();
                        boolean xIns = xErr == null && xOp.steps.size() == 5
                                && xOp.steps.get(1).type == Ops.T_IF
                                && xOp.steps.get(2).type == Ops.T_CLAUSE && xOp.steps.get(2).a1 == Ops.J_AND
                                && xOp.steps.get(2).a2 == 4 && xOp.steps.get(2).a3 == -6
                                && xOp.steps.get(3).type == Ops.T_ELSE && xOp.steps.get(4).type == Ops.T_ENDIF
                                && Ops.clauseCount(xOp.steps, 1) == 2 && Ops.structureErr(xOp.steps) == null;
                        xPg.openStepForTest(1);                            // 编辑已有块：组替换保序
                        String xErr2 = xPg.saveStepEdit();
                        boolean xEdit2 = xErr2 == null && xOp.steps.size() == 5 && xOp.steps.get(2).a3 == -6
                                && Ops.structureErr(xOp.steps) == null;
                        String xErr3 = xPg.insertNewStep(-1, bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0), "",
                                java.util.Arrays.asList(bstep(Ops.T_CLAUSE, Ops.J_AND, 0, 0, 0, 0, "r1", "", 0, 0)));
                        boolean xRej = xErr3 != null && xOp.steps.size() == 5;             // 次数循环拒挂子条件
                        android.widget.LinearLayout xWrap = Ui.col(ctx);
                        xWrap.setBackgroundColor(Ui.BG);
                        xWrap.addView(xRoot, new android.widget.LinearLayout.LayoutParams(
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                        renderView(ctx, xWrap, "edit_ops_addclause.png");
                        boolean xTxt = containsText(xRoot, "如果 · 变量 fx > 0") && containsText(xRoot, "且 · 变量 fx > 0");
                        rec(out, logFile, (xAdd && xTap && xIns && xEdit2 && xRej && xTxt ? "[OK] " : "[FAIL] ")
                                + "新增块带子条件核 v32：同页加子条件=" + xAdd + " 保存随插=" + xIns
                                + " 编辑组替换=" + xEdit2 + " 非到条件拒=" + (xErr3 != null)
                                + " 渲染关键文本=" + xTxt + "（edit_ops_addclause.png）");
                    } catch (Throwable t) {
                        rec(out, logFile, "[FAIL] 新增块带子条件核异常: " + t);
                    }

                    /* v33：下方插入核——统一落点（矩阵+破损）+ 行「＋」直驱（普通/块头/块插入/否则/块尾/末尾/取消）+ 渲染件 */
                    try {
                        OpsPage h33Pg = new OpsPage(ctx);
                        View h33Root = h33Pg.build();
                        Ops.Op h33Op = new Ops.Op();
                        h33Op.name = "下方";
                        h33Op.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));            // 0 普通
                        h33Op.steps.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));             // 1 条件块头
                        h33Op.steps.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0)); // 2 子条件
                        h33Op.steps.add(bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0));         // 3 成立分支
                        h33Op.steps.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));             // 4 否则
                        h33Op.steps.add(bstep(Ops.T_TAP, 300, 400, 0, 0, 50, "", "", 0, 0));         // 5 否则分支
                        h33Op.steps.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));            // 6 块尾
                        h33Op.steps.add(bstep(Ops.T_LOOP, 0, 3, 0, 0, 0, "", "", 0, 0));             // 7 循环头
                        h33Op.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 80, "", "", 0, 0));            // 8 循环体
                        h33Op.steps.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0));          // 9 循环结束
                        h33Op.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 120, "", "", 0, 0));           // 10 末尾
                        h33Pg.openEditorForTest(h33Op);
                        boolean h1 = Ops.insertBelowAt(h33Op.steps, 0) == 1
                                && Ops.insertBelowAt(h33Op.steps, 1) == 3
                                && Ops.insertBelowAt(h33Op.steps, 2) == -1
                                && Ops.insertBelowAt(h33Op.steps, 4) == 5
                                && Ops.insertBelowAt(h33Op.steps, 6) == 7
                                && Ops.insertBelowAt(h33Op.steps, 7) == 8
                                && Ops.insertBelowAt(h33Op.steps, 10) == 11
                                && "成立分支开头".equals(Ops.insertBelowCtx(h33Op.steps, 1))
                                && "整块之后".equals(Ops.insertBelowCtx(h33Op.steps, 9))
                                && "".equals(Ops.insertBelowCtx(h33Op.steps, 0));
                        List<Ops.Step> h33Bad = new ArrayList<Ops.Step>();
                        h33Bad.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));
                        boolean h2 = Ops.insertBelowAt(h33Bad, 0) == -1 && Ops.structureErr(h33Op.steps) == null;
                        android.widget.LinearLayout h33Wrap = Ui.col(ctx);
                        h33Wrap.setBackgroundColor(Ui.BG);
                        h33Wrap.addView(h33Root, new android.widget.LinearLayout.LayoutParams(
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                        renderView(ctx, h33Wrap, "edit_ops_addbelow.png");
                        int h33Plus = textCount(h33Root, "＋");
                        boolean h33a = h33Plus == 10;              // 普通行×5 + 块头×2 + 否则×1 + 块尾×2
                        boolean h33b = !containsText(h33Root, "✎ 编辑")
                                && containsText(h33Root, "＋ 添加步骤");
                        /* 加拍：无否则条件块 + 两个块尾行（[＋] 与 [＋ 否则] 并排、[＋][✕ 整块] 可见） */
                        OpsPage h33Pg3 = new OpsPage(ctx);
                        View h33Root3 = h33Pg3.build();
                        Ops.Op h33Op3 = new Ops.Op();
                        h33Op3.name = "块尾";
                        h33Op3.steps.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));    // 0 头（无否则）
                        h33Op3.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));  // 1 体
                        h33Op3.steps.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));   // 2 尾：[＋][＋ 否则][✕ 整块]
                        h33Op3.steps.add(bstep(Ops.T_LOOP, 0, 2, 0, 0, 0, "", "", 0, 0));    // 3 头
                        h33Op3.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 80, "", "", 0, 0));   // 4 体
                        h33Op3.steps.add(bstep(Ops.T_ENDLOOP, 0, 0, 0, 0, 0, "", "", 0, 0)); // 5 尾
                        h33Pg3.openEditorForTest(h33Op3);
                        android.widget.LinearLayout h33Wrap3 = Ui.col(ctx);
                        h33Wrap3.setBackgroundColor(Ui.BG);
                        h33Wrap3.addView(h33Root3, new android.widget.LinearLayout.LayoutParams(
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                        renderView(ctx, h33Wrap3, "edit_ops_addbelow_tail.png");
                        int h33Plus3 = textCount(h33Root3, "＋");
                        boolean h33c = h33Plus3 == 6 && containsText(h33Root3, "＋ 否则");
                        boolean h3 = h33a && h33b && h33c;
                        /* 直驱：与行「＋」同源（弹层选型 → 保存落位）；夹具每步后还原 */
                        OpsPage h33Pg2 = new OpsPage(ctx);
                        h33Pg2.build();
                        Ops.Op h33Op2 = new Ops.Op();
                        h33Op2.name = "下方驱";
                        h33Op2.steps.add(bstep(Ops.T_IF, 0, 5, 6, 0, 0, "r9", "", 0, 0));             // 0 头
                        h33Op2.steps.add(bstep(Ops.T_CLAUSE, Ops.J_AND, 1, 0, 0, 0, "s1", "", 0, 0)); // 1 子条件
                        h33Op2.steps.add(bstep(Ops.T_TAP, 100, 200, 0, 0, 50, "", "", 0, 0));         // 2 体
                        h33Op2.steps.add(bstep(Ops.T_ELSE, 0, 0, 0, 0, 0, "", "", 0, 0));             // 3 否则
                        h33Op2.steps.add(bstep(Ops.T_ENDIF, 0, 0, 0, 0, 0, "", "", 0, 0));            // 4 尾
                        h33Op2.steps.add(bstep(Ops.T_SWIPE, 10, 20, 30, 40, 100, "", "", 0, 0));      // 5 尾后
                        h33Pg2.openEditorForTest(h33Op2);
                        h33Pg2.startNewStepForTest(Ops.T_WAIT, Ops.insertBelowAt(h33Op2.steps, 2),
                                Ops.insertBelowCtx(h33Op2.steps, 2));
                        String h33e1 = h33Pg2.saveStepEdit();
                        boolean h4 = h33e1 == null && h33Op2.steps.get(3).type == Ops.T_WAIT
                                && h33Op2.steps.get(4).type == Ops.T_ELSE && Ops.blockHeadOf(h33Op2.steps, 3) == 0
                                && h33Pg2.focusIdxRequest == 3 && Ops.structureErr(h33Op2.steps) == null;
                        h33Op2.steps.remove(3);
                        h33Pg2.startNewStepForTest(Ops.T_WAIT, Ops.insertBelowAt(h33Op2.steps, 0),
                                Ops.insertBelowCtx(h33Op2.steps, 0));
                        String h33e2 = h33Pg2.saveStepEdit();
                        boolean h5 = h33e2 == null && h33Op2.steps.get(2).type == Ops.T_WAIT
                                && h33Op2.steps.get(3).type == Ops.T_TAP && Ops.blockHeadOf(h33Op2.steps, 2) == 0
                                && h33Pg2.focusIdxRequest == 2 && Ops.structureErr(h33Op2.steps) == null;
                        h33Op2.steps.remove(2);
                        h33Pg2.startNewStepForTest(Ops.T_LOOP, Ops.insertBelowAt(h33Op2.steps, 0),
                                Ops.insertBelowCtx(h33Op2.steps, 0));
                        String h33e3 = h33Pg2.saveStepEdit();
                        boolean h6 = h33e3 == null && h33Op2.steps.get(2).type == Ops.T_LOOP
                                && h33Op2.steps.get(3).type == Ops.T_ENDLOOP
                                && Ops.blockHeadOf(h33Op2.steps, 2) == 0 && Ops.structureErr(h33Op2.steps) == null;
                        h33Op2.steps.remove(3); h33Op2.steps.remove(2);
                        h33Pg2.startNewStepForTest(Ops.T_WAIT, Ops.insertBelowAt(h33Op2.steps, 3),
                                Ops.insertBelowCtx(h33Op2.steps, 3));
                        String h33e4 = h33Pg2.saveStepEdit();
                        boolean h7 = h33e4 == null && h33Op2.steps.get(4).type == Ops.T_WAIT
                                && h33Op2.steps.get(5).type == Ops.T_ENDIF && Ops.structureErr(h33Op2.steps) == null;
                        h33Op2.steps.remove(4);
                        h33Pg2.startNewStepForTest(Ops.T_WAIT, Ops.insertBelowAt(h33Op2.steps, 4),
                                Ops.insertBelowCtx(h33Op2.steps, 4));
                        String h33e5 = h33Pg2.saveStepEdit();
                        boolean h8 = h33e5 == null && h33Op2.steps.get(5).type == Ops.T_WAIT
                                && h33Op2.steps.get(6).type == Ops.T_SWIPE && Ops.blockHeadOf(h33Op2.steps, 5) == -1
                                && Ops.structureErr(h33Op2.steps) == null;
                        h33Op2.steps.remove(5);
                        h33Pg2.startNewStepForTest(Ops.T_WAIT, Ops.insertBelowAt(h33Op2.steps, 5),
                                Ops.insertBelowCtx(h33Op2.steps, 5));
                        String h33e6 = h33Pg2.saveStepEdit();
                        boolean h9 = h33e6 == null && h33Op2.steps.size() == 7 && h33Op2.steps.get(6).type == Ops.T_WAIT
                                && h33Pg2.focusIdxRequest == 6 && Ops.structureErr(h33Op2.steps) == null;
                        h33Op2.steps.remove(6);
                        h33Pg2.startNewStepForTest(Ops.T_WAIT, 1, "");                 // 取消 = 不插入
                        h33Pg2.cancelStepEdit();
                        boolean h10 = h33Op2.steps.size() == 6 && h33Pg2.stepEditorForTest() == null;
                        rec(out, logFile, (h1 && h2 && h3 && h4 && h5 && h6 && h7 && h8 && h9 && h10
                                ? "[OK] " : "[FAIL] ")
                                + "下方插入核 v33：落点矩阵=" + h1 + " 破损拒=" + h2 + " 渲染（＋×" + h33Plus
                                + " 无编辑键=" + h33b + " 块尾双＋=" + h33c + "）=" + h3 + " 普通直驱=" + h4 + " 块头直驱=" + h5
                                + " 块插入=" + h6 + " 否则直驱=" + h7 + " 块尾直驱=" + h8
                                + " 末尾追加=" + h9 + " 取消不插入=" + h10 + "（edit_ops_addbelow.png）");
                    } catch (Throwable t) {
                        rec(out, logFile, "[FAIL] 下方插入核异常: " + t);
                    }

                    // ① 会话保留核：applyLoad（切页/刷新路径）不清编辑现场；Shell 切页往返仍在编辑
                    OpsPage sPg = new OpsPage(ctx);
                    View sRoot = sPg.build();
                    Ops.Op sOp = new Ops.Op();
                    sOp.name = "保留";
                    sOp.steps.add(bstep(Ops.T_TAP, 10, 20, 0, 0, 50, "", "", 0, 0));
                    sPg.openEditorForTest(sOp);
                    boolean s1 = sPg.busy() && containsText(sRoot, "编辑中：操作 保留");
                    sPg.applyLoad("#vtouch-ops v5\nop 保留 gate - autooff 0\nstep 1 10 20 0 0 50 - 0 0 -\n"
                                    + "op 其它 gate - autooff 0\nstep 1 1 2 0 0 50 - 0 0 -\n",
                            new ArrayList<String>(), new ArrayList<String>(), null, null,
                            new ArrayList<Regions.Region>());
                    boolean s2 = sPg.busy() && containsText(sRoot, "编辑中：操作 保留")
                            && !containsText(sRoot, "共 2 个操作");
                    Shell sSh = new Shell();
                    sSh.build(ctx);
                    sSh.showTab(1);                                    // 初次进入（非编辑态：正常重载）
                    sSh.opsPage.openEditorForTest(sOp);                // 直驱进入编辑
                    sSh.showTab(0);                                    // 切走
                    sSh.showTab(1);                                    // 切回（编辑中 → 跳过重载，现场保留）
                    boolean s3 = sSh.opsPage.busy() && containsText(sSh.root, "编辑中：操作 保留");
                    rec(out, logFile, (s1 && s2 && s3 ? "[OK] " : "[FAIL] ")
                            + "会话保留 v26：切页/刷新不打断编辑（进入编辑=" + s1 + " 刷新后仍在编辑=" + s2
                            + " 切页往返=" + s3 + "）");

                    // ② 添加定位核：末尾插入第 41 步 → 请求定位且目标行可滚入视野（离屏 measure/layout 后核对）
                    OpsPage fPg = new OpsPage(ctx);
                    View fRoot = fPg.build();
                    Ops.Op fOp = new Ops.Op();
                    fOp.name = "定位";
                    for (int k = 0; k < 40; k++)
                        fOp.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 50 + k, "", "", 0, 0));
                    fPg.openEditorForTest(fOp);
                    android.widget.LinearLayout fWrap = Ui.col(ctx);
                    fWrap.setBackgroundColor(Ui.BG);
                    fWrap.addView(fRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    int fw = ctx.getResources().getDisplayMetrics().widthPixels;
                    int fh = ctx.getResources().getDisplayMetrics().heightPixels - Ui.dp(ctx, 40);
                    fWrap.measure(View.MeasureSpec.makeMeasureSpec(fw, View.MeasureSpec.EXACTLY),
                            View.MeasureSpec.makeMeasureSpec(fh, View.MeasureSpec.EXACTLY));
                    fWrap.layout(0, 0, fw, fh);
                    String fErr = fPg.insertNewStep(-1, bstep(Ops.T_WAIT, 0, 0, 0, 0, 99, "", "", 0, 0), "");
                    boolean fWire = fErr == null && fPg.focusIdxRequest == 40;
                    int fEarly = fPg.applyFocusNow();                       // 布局前：不瞎滚、请求保留（待重试）
                    boolean fRetry = fEarly == -1 && fPg.focusIdxRequest == 40;
                    fWrap.measure(View.MeasureSpec.makeMeasureSpec(fw, View.MeasureSpec.EXACTLY),
                            View.MeasureSpec.makeMeasureSpec(fh, View.MeasureSpec.EXACTLY));
                    fWrap.layout(0, 0, fw, fh);
                    int[] fm = fPg.focusMetricsForTest(40);
                    boolean fVis = fm[0] >= 0 && fm[3] > 0
                            && fm[0] - fm[3] >= -Ui.dp(ctx, 13)
                            && fm[1] - fm[3] <= fm[2] + Ui.dp(ctx, 13) && fm[2] > 0;
                    int fY = fPg.applyFocusNow();
                    boolean fExec = fY == fm[3] && fPg.focusIdxRequest == -1;
                    rec(out, logFile, (fWire && fRetry && fVis && fExec ? "[OK] " : "[FAIL] ")
                            + "添加定位 v26：插入后请求行=" + fWire + " 未量测重试核=" + fRetry
                            + " 行可滚入视野=" + fVis + "（目标 Y=" + fm[3] + "） 执行=" + fExec);

                    // ③ 返回定位核 v27：步骤编辑返回恢复进入前滚动位置（不回顶）
                    OpsPage yPg = new OpsPage(ctx);
                    View yRoot = yPg.build();
                    Ops.Op yOp = new Ops.Op();
                    yOp.name = "回位";
                    for (int k = 0; k < 40; k++)
                        yOp.steps.add(bstep(Ops.T_WAIT, 0, 0, 0, 0, 50 + k, "", "", 0, 0));
                    yPg.openEditorForTest(yOp);
                    android.widget.LinearLayout yWrap = Ui.col(ctx);
                    yWrap.setBackgroundColor(Ui.BG);
                    yWrap.addView(yRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    yWrap.measure(View.MeasureSpec.makeMeasureSpec(fw, View.MeasureSpec.EXACTLY),
                            View.MeasureSpec.makeMeasureSpec(fh, View.MeasureSpec.EXACTLY));
                    yWrap.layout(0, 0, fw, fh);
                    yPg.listScrollToForTest(Ui.dp(ctx, 420));
                    int capGot = yPg.listScrollYForTest();
                    yPg.openStepForTest(25);                                  // 进入步骤编辑（捕获位置）
                    boolean yCap = capGot > 0 && yPg.stepReturnYForTest() == capGot;
                    yPg.cancelStepEdit();                                     // 返回（取消路径）
                    boolean yWire = yPg.restoreYRequestForTest() == capGot;
                    yWrap.measure(View.MeasureSpec.makeMeasureSpec(fw, View.MeasureSpec.EXACTLY),
                            View.MeasureSpec.makeMeasureSpec(fh, View.MeasureSpec.EXACTLY));
                    yWrap.layout(0, 0, fw, fh);                               // 返回后列表重建 → 重新布局
                    int yHit = yPg.applyRestoreNow();                         // 立即执行（真实路径由 post 驱动）
                    boolean yDone = yHit == capGot && yPg.listScrollYForTest() == yHit
                            && yPg.restoreYRequestForTest() == -1;
                    rec(out, logFile, (yCap && yWire && yDone ? "[OK] " : "[FAIL] ")
                            + "返回定位 v27：进入前捕获=" + yCap + " 返回即请求=" + yWire + " 恢复生效=" + yDone
                            + "（Y=" + capGot + "→" + yHit + "）");

                    // ④ 提示 v6D 核：模板校验镜像 / 往返 / 摘要 / 编辑器渲染件
                    String[] tslots = new String[Ops.VAR_N];
                    tslots[0] = "e9";
                    Ops.Step seT = bstep(Ops.T_TOAST, 0, 0, 0, 0, 3000, "", "", 0, 0);
                    seT.expr = "已连击 {e9} 次 · fx={fx}";
                    Ops.Op tOp = new Ops.Op();
                    tOp.name = "提示t";
                    tOp.gate = "-";
                    Ops.Step cs = bstep(Ops.T_CALC, 0, 0, 0, 0, 0, "e9", "", 0, 0);
                    cs.expr = "5";
                    tOp.steps.add(cs);
                    tOp.steps.add(seT);
                    Ops.File tF = new Ops.File();
                    tF.ops.add(tOp);
                    boolean tRound = false;
                    try {
                        Ops.File tF2 = Ops.File.parse(tF.serialize());
                        Ops.Op t2 = tF2.find("提示t");
                        tRound = t2 != null && t2.steps.size() == 2 && t2.steps.get(1).type == Ops.T_TOAST
                                && "已连击 {e9} 次 · fx={fx}".equals(t2.steps.get(1).expr)
                                && t2.steps.get(1).ms == 3000 && Ops.validate(t2, tF2, t2) == null;
                    } catch (Throwable ig) { }
                    boolean tPos = ctrlValOf("提示t", "9 0 0 0 0 0 e9 0 0 5",
                            "21 0 0 0 0 3000 - 0 0 已点击 {e9} 次") == null;
                    boolean tNeg = ctrlRej(ctrlValOf("提示t", "21 0 0 0 0 2000 - 0 0 未闭合 {tdx"), "未闭合")
                            && ctrlRej(ctrlValOf("提示t", "21 0 0 0 0 2000 - 0 0 {nope}"), "未知变量")
                            && ctrlRej(ctrlValOf("提示t", "21 0 0 0 0 2000 - 0 0 -"), "模板为空")
                            && ctrlRej(ctrlValOf("提示t", "21 1 0 0 0 2000 - 0 0 hi"), "字段必须为 0")
                            && ctrlRej(ctrlValOf("提示t", "21 0 0 0 0 70000 - 0 0 hi"), "时长越界");
                    Ops.Step sy = bstep(Ops.T_TOAST, 0, 0, 0, 0, 2000, "", "", 0, 0);
                    sy.expr = "已点击 {e9} 次";
                    boolean tSum = sy.summary().startsWith("提示 · 已点击");
                    StepEdit seTv = StepEdit.build(ctx, seT, idsList, idsList, idsList, idsList,
                            tslots, null, null);
                    renderWrap(ctx, seTv.root, "edit_step_prompt.png", Ui.BG);
                    boolean tUi = containsText(seTv.root, "提示文本") && containsText(seTv.root, "示例预览")
                            && containsText(seTv.root, "插入变量") && containsText(seTv.root, "显示时长")
                            && containsText(seTv.root, "已连击 5 次") && containsText(seTv.root, "fx=5");
                    rec(out, logFile, (tRound && tPos && tNeg && tSum && tUi ? "[OK] " : "[FAIL] ")
                            + "提示 v6D 核：往返=" + tRound + " 正例=" + tPos + " 负例五门=" + tNeg
                            + " 摘要=" + tSum + " 渲染件 edit_step_prompt.png=" + tUi);
                    // ⑤ 框区域 v6E 核：往返 / 校验镜像 / 摘要 / 编辑器渲染件
                    Ops.Step seH = bstep(Ops.T_HL, 0, 0, 0, 0, 3000, "c1", "", 0, 0);
                    Ops.Op hOp = new Ops.Op();
                    hOp.name = "框区域t";
                    hOp.gate = "-";
                    hOp.steps.add(seH);
                    Ops.File hF = new Ops.File();
                    hF.ops.add(hOp);
                    boolean hRound = false;
                    try {
                        Ops.File hF2 = Ops.File.parse(hF.serialize());
                        Ops.Op h2 = hF2.find("框区域t");
                        hRound = h2 != null && h2.steps.size() == 1 && h2.steps.get(0).type == Ops.T_HL
                                && "c1".equals(h2.steps.get(0).ref) && h2.steps.get(0).ms == 3000
                                && Ops.validate(h2, hF2, h2) == null;
                    } catch (Throwable ig) { }
                    boolean hPos = ctrlValOf("框区域t", "22 0 0 0 0 3000 c1 0 0 -") == null;
                    boolean hNeg = ctrlRej(ctrlValOf("框区域t", "22 0 0 0 0 3000 - 0 0 -"), "区域引用非法")
                            && ctrlRej(ctrlValOf("框区域t", "22 0 0 0 0 70000 c1 0 0 -"), "时长越界")
                            && ctrlRej(ctrlValOf("框区域t", "22 1 0 0 0 3000 c1 0 0 -"), "字段必须为 0")
                            && ctrlRej(ctrlValOf("框区域t", "22 0 0 0 0 3000 c1 0 0 x"), "表达式错");
                    boolean hSum = false;
                    { Ops.Step sy2 = bstep(Ops.T_HL, 0, 0, 0, 0, 3000, "c1", "", 0, 0);
                      hSum = sy2.summary().startsWith("框区域") && sy2.summary().contains("c1"); }
                    StepEdit seHv = StepEdit.build(ctx, seH, idsList, idsList, idsList, idsList,
                            new String[Ops.VAR_N], null, null);
                    renderWrap(ctx, seHv.root, "edit_step_box.png", Ui.BG);
                    boolean hUi = containsText(seHv.root, "区域 id") && containsText(seHv.root, "c1")
                            && containsText(seHv.root, "显示时长") && !containsText(seHv.root, "暂无区域");
                    rec(out, logFile, (hRound && hPos && hNeg && hSum && hUi ? "[OK] " : "[FAIL] ")
                            + "框区域 v6E 核：往返=" + hRound + " 正例=" + hPos + " 负例四门=" + hNeg
                            + " 摘要=" + hSum + " 渲染件 edit_step_box.png=" + hUi);
                    // ⑥ 框矩形 v6F 核：往返 / 校验镜像 / 摘要 / 编辑器渲染件
                    Ops.Step seB = bstep(Ops.T_BOX, 200, 300, 900, 800, 3000, "", "", 0, 0);
                    Ops.Op bOp6 = new Ops.Op();
                    bOp6.name = "框矩形t";
                    bOp6.gate = "-";
                    bOp6.steps.add(seB);
                    Ops.File bF = new Ops.File();
                    bF.ops.add(bOp6);
                    boolean bRound = false;
                    try {
                        Ops.File bF2 = Ops.File.parse(bF.serialize());
                        Ops.Op b2 = bF2.find("框矩形t");
                        bRound = b2 != null && b2.steps.size() == 1 && b2.steps.get(0).type == Ops.T_BOX
                                && b2.steps.get(0).a1 == 200 && b2.steps.get(0).a4 == 800
                                && Ops.validate(b2, bF2, b2) == null;
                    } catch (Throwable ig) { }
                    boolean bPos = ctrlValOf("框矩形t", "23 200 300 900 800 3000 - 0 0 -") == null
                            && ctrlValOf("框矩形t", "23 -6 -7 900 800 3000 - 0 0 -") == null;
                    boolean bNeg = ctrlRej(ctrlValOf("框矩形t", "23 -26 300 900 800 3000 - 0 0 -"), "坐标越界")
                            && ctrlRej(ctrlValOf("框矩形t", "23 200 300 900 800 70000 - 0 0 -"), "时长越界")
                            && ctrlRej(ctrlValOf("框矩形t", "23 200 300 900 800 3000 c1 0 0 -"), "字段必须为 0");
                    boolean bSum = false;
                    { Ops.Step syB = bstep(Ops.T_BOX, 200, 300, 900, 800, 3000, "", "", 0, 0);
                      bSum = syB.summary().startsWith("框矩形") && syB.summary().contains("200"); }
                    StepEdit seBv = StepEdit.build(ctx, seB, idsList, idsList, idsList, idsList,
                            new String[Ops.VAR_N], null, null);
                    renderWrap(ctx, seBv.root, "edit_step_boxcoords.png", Ui.BG);
                    boolean bUi = containsText(seBv.root, "框矩形") && containsText(seBv.root, "x1")
                            && containsText(seBv.root, "y1") && containsText(seBv.root, "200")
                            && containsText(seBv.root, "显示时长");
                    rec(out, logFile, (bRound && bPos && bNeg && bSum && bUi ? "[OK] " : "[FAIL] ")
                            + "框矩形 v6F 核：往返=" + bRound + " 正例=" + bPos + " 负例三门=" + bNeg
                            + " 摘要=" + bSum + " 渲染件 edit_step_boxcoords.png=" + bUi);
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] v26 三步核异常: " + t);
                }
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

                /* v35（T6 双视图）：脚本编辑渲染件 edit_script.png / edit_script_err.png / edit_script_long.png */
                try {
                    String scr = "/data/local/tmp/vte-t6scratch2";
                    Su.run("rm -rf '" + scr + "'");
                    OpsV8.baseOverride = scr;
                    Su.run("mkdir -p '" + scr + "/progs'");
                    String sSrc = "#模式 脚本\n如果 在区域(\"c1\", tux, tuy) {\n    等待 100\n} 否则 {\n    点按 540 1584 50\n}\n";
                    OpsV8.writeSrc(scr, "脚本样", sSrc);
                    ScriptStore.Save sV = ScriptStore.prepare(sSrc, "脚本");
                    OpsV8.writeProg(scr, "脚本样", sV.bytes);
                    String sConf = "#vtouch-ops v8\nop 脚本样 gate - autooff 0 prog " + OpsV8.fnv8(sV.bytes) + " " + sV.bytes.length + "\n";
                    Su.writeAtomic(scr + "/ops.conf", sConf.getBytes("UTF-8"));

                    OpsPage sPg = new OpsPage(ctx);
                    View sRoot = sPg.build();
                    sPg.pathOverride = scr + "/ops.conf";
                    sPg.applyLoad(sConf, new ArrayList<String>(), new ArrayList<String>(), null, null,
                            new ArrayList<Regions.Region>());
                    android.widget.LinearLayout sw = Ui.col(ctx);
                    sw.setBackgroundColor(Ui.BG);
                    sw.addView(sRoot, new android.widget.LinearLayout.LayoutParams(
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                            android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                    String sErr = sPg.startOpForTest(0);
                    ScriptEdit sEd = sPg.scriptEditorForTest();
                    boolean ok1 = sErr == null && sEd != null && "脚本".equals(sPg.editModeForTest());
                    sEd.setTextForTest("#模式 脚本\n如果 在区域(\"c1\", tux, tuy) {\n    等待 100\n}\n");
                    sEd.refreshNowForTest();
                    renderView(ctx, sw, "edit_script.png");
                    List<String> sv1 = new ArrayList<String>();
                    assertOverflow(sw, "脚本编辑", sv1, ctx);
                    boolean t1 = sEd.kwSpanCountForTest() >= 3 && sEd.assistKeyCountForTest() > 15
                            && sEd.gutterWidthForTest() > 0 && containsText(sRoot, "脚本编辑：脚本样")
                            && containsText(sRoot, "格式化") && containsText(sRoot, "等待 100")
                            && containsText(sRoot, "帧上取点") && containsText(sRoot, "屏幕取点");
                    /* 错误态：第 3 行起错 → 行底标红 + 错误条（第 3 行） */
                    sEd.setTextForTest("#模式 脚本\n点按 100 200 50\n循环 {\n");
                    int erl = sEd.refreshNowForTest();
                    renderView(ctx, sw, "edit_script_err.png");
                    boolean t2 = erl == 3 && sEd.errorMsgForTest() != null && sEd.errBgSpanCountForTest() >= 1
                            && containsText(sRoot, "第 3 行");
                    /* 长文滚动核 + 行号槽对齐件（滚至中段取景） */
                    StringBuilder longT = new StringBuilder("#模式 脚本\n");
                    for (int k = 0; k < 60; k++) longT.append("等待 ").append(100 + k).append("\n");
                    sEd.setTextForTest(longT.toString());
                    sEd.refreshNowForTest();
                    int lw2 = ctx.getResources().getDisplayMetrics().widthPixels;
                    int lh2 = ctx.getResources().getDisplayMetrics().heightPixels - Ui.dp(ctx, 40);
                    sw.measure(View.MeasureSpec.makeMeasureSpec(lw2, View.MeasureSpec.EXACTLY),
                            View.MeasureSpec.makeMeasureSpec(lh2, View.MeasureSpec.EXACTLY));
                    sw.layout(0, 0, lw2, lh2);
                    boolean t3pre = sEd.scrollableForTest();
                    sEd.scrollByForTest(400);
                    boolean t3 = t3pre && sEd.scrollYForTest() > 0;
                    /* 拖拽滚动核（合成 MotionEvent 直投 View 层；不进系统输入）：拖住编区往上划 → 应滚动 */
                    int y0s = sEd.scrollYForTest();
                    long tt = android.os.SystemClock.uptimeMillis();
                    android.view.MotionEvent mdn = android.view.MotionEvent.obtain(tt, tt,
                            android.view.MotionEvent.ACTION_DOWN, Ui.dp(ctx, 120), lh2 / 2, 0);
                    sw.dispatchTouchEvent(mdn);
                    mdn.recycle();
                    for (int k = 1; k <= 10; k++) {
                        android.view.MotionEvent mv = android.view.MotionEvent.obtain(tt, tt + k * 16,
                                android.view.MotionEvent.ACTION_MOVE, Ui.dp(ctx, 120), lh2 / 2 - k * 70, 0);
                        sw.dispatchTouchEvent(mv);
                        mv.recycle();
                    }
                    android.view.MotionEvent mup = android.view.MotionEvent.obtain(tt, tt + 220,
                            android.view.MotionEvent.ACTION_UP, Ui.dp(ctx, 120), lh2 / 2 - 700, 0);
                    sw.dispatchTouchEvent(mup);
                    mup.recycle();
                    boolean t4 = sEd.scrollYForTest() != y0s;
                    renderView(ctx, sw, "edit_script_long.png");
                    /* v37：输入提示条渲染 + 断言（exact 作用参数态 / 前缀候选态 / 点选补全 / 串内抑制） */
                    sEd.setTextForTest("点按 100\n");
                    sEd.cursorToForTest(2);
                    String htxt = sEd.hintTextForTest();
                    boolean h1 = htxt != null && htxt.contains("点按") && htxt.contains("点击");
                    renderView(ctx, sw, "edit_script_hint.png");
                    sEd.setTextForTest("找");
                    sEd.cursorEndForTest();
                    boolean h2 = sEd.hintCandCountForTest() == 3;
                    sEd.tapCandForTest(0);
                    boolean h3 = sEd.textForTest().startsWith("找图");
                    sEd.setTextForTest("提示 \"等待\n");
                    sEd.cursorToForTest(6);
                    boolean h4 = sEd.hintTextForTest() == null;
                    boolean hk = h1 && h2 && h3 && h4;

                    /* v38：双指捏合缩放核——真手势路径（合成双指事件）+ 钳制/行号槽跟缩/记忆 + 单指回归 */
                    boolean z1 = false, z2 = false, z3 = false, z4 = false, z5 = false, z6 = false, z7 = false, zk = false;
                    float zgSeen = 13f;
                    ScriptEdit.zoomPersistSuppressedForTest = true;
                    android.content.SharedPreferences zPref = ctx.getSharedPreferences(
                            ScriptEdit.PREF_FILE, android.content.Context.MODE_PRIVATE);
                    final float zOld = zPref.getFloat(ScriptEdit.PREF_KEY, 13f);
                    try {
                        zPref.edit().putFloat(ScriptEdit.PREF_KEY, 13f).apply();   // 已知初值（判「记忆读取」）
                        android.widget.LinearLayout swz = Ui.col(ctx);
                        swz.setBackgroundColor(Ui.BG);
                        ScriptEdit sz = ScriptEdit.build(ctx, "", "门控 无 · 自动关 关", null);
                        swz.addView(sz.root, new android.widget.LinearLayout.LayoutParams(
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT,
                                android.widget.LinearLayout.LayoutParams.MATCH_PARENT));
                        sz.setTextForTest("#模式 脚本\n如果 在区域(\"c1\", tux, tuy) {\n    等待 100\n}\n");
                        sz.refreshNowForTest();
                        swz.measure(View.MeasureSpec.makeMeasureSpec(lw2, View.MeasureSpec.EXACTLY),
                                View.MeasureSpec.makeMeasureSpec(lh2, View.MeasureSpec.EXACTLY));
                        swz.layout(0, 0, lw2, lh2);
                        z1 = sz.fontSizeSpForTest() == 13f;
                        int zw1 = sz.gutterWidthForTest();

                        /* 合成双指（真手势路径）：DOWN → POINTER_DOWN → MOVE×3（跨度 400→640px）→ POINTER_UP → UP */
                        android.view.MotionEvent.PointerProperties[] zpp = new android.view.MotionEvent.PointerProperties[2];
                        android.view.MotionEvent.PointerCoords[] zpc = new android.view.MotionEvent.PointerCoords[2];
                        for (int i = 0; i < 2; i++) {
                            zpp[i] = new android.view.MotionEvent.PointerProperties();
                            zpp[i].id = i;
                            zpp[i].toolType = android.view.MotionEvent.TOOL_TYPE_FINGER;
                            zpc[i] = new android.view.MotionEvent.PointerCoords();
                        }
                        long zt = android.os.SystemClock.uptimeMillis();
                        float zy = lh2 / 2f, zxa = Ui.dp(ctx, 120), zxb = Ui.dp(ctx, 220);
                        zpc[0].x = zxa; zpc[0].y = zy;
                        android.view.MotionEvent ze = android.view.MotionEvent.obtain(zt, zt,
                                android.view.MotionEvent.ACTION_DOWN, 1, zpp, zpc, 0, 0, 1f, 1f, 0, 0,
                                android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
                        swz.dispatchTouchEvent(ze);
                        ze.recycle();
                        zpc[1].x = zxb; zpc[1].y = zy;
                        ze = android.view.MotionEvent.obtain(zt, zt + 16,
                                android.view.MotionEvent.ACTION_POINTER_DOWN
                                        | (1 << android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT), 2, zpp, zpc, 0, 0, 1f, 1f, 0, 0,
                                android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
                        swz.dispatchTouchEvent(ze);
                        ze.recycle();
                        float[] zdz = { 40f, 80f, 120f };
                        for (int k = 0; k < 3; k++) {
                            zpc[0].x = zxa - zdz[k];
                            zpc[1].x = zxb + zdz[k];
                            ze = android.view.MotionEvent.obtain(zt, zt + 16 * (k + 2),
                                    android.view.MotionEvent.ACTION_MOVE, 2, zpp, zpc, 0, 0, 1f, 1f, 0, 0,
                                    android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
                            swz.dispatchTouchEvent(ze);
                            ze.recycle();
                        }
                        ze = android.view.MotionEvent.obtain(zt, zt + 80,
                                android.view.MotionEvent.ACTION_POINTER_UP
                                        | (1 << android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT), 2, zpp, zpc, 0, 0, 1f, 1f, 0, 0,
                                android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
                        swz.dispatchTouchEvent(ze);
                        ze.recycle();
                        ze = android.view.MotionEvent.obtain(zt, zt + 96,
                                android.view.MotionEvent.ACTION_UP, 1, zpp, zpc, 0, 0, 1f, 1f, 0, 0,
                                android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
                        swz.dispatchTouchEvent(ze);
                        ze.recycle();
                        zgSeen = sz.fontSizeSpForTest();
                        z2 = zgSeen > 14f && zgSeen <= ScriptEdit.ZOOM_MAX
                                && Math.abs(zgSeen * 2f - Math.round(zgSeen * 2f)) < 0.001f;

                        /* 单指回归：捏合后单指拖拽仍滚动（无卡死状态；长文保证有滚动量） */
                        sz.setTextForTest(longT.toString());
                        sz.refreshNowForTest();
                        swz.measure(View.MeasureSpec.makeMeasureSpec(lw2, View.MeasureSpec.EXACTLY),
                                View.MeasureSpec.makeMeasureSpec(lh2, View.MeasureSpec.EXACTLY));
                        swz.layout(0, 0, lw2, lh2);
                        int zy0 = sz.scrollYForTest();
                        ze = android.view.MotionEvent.obtain(zt, zt + 200, android.view.MotionEvent.ACTION_DOWN,
                                Ui.dp(ctx, 120), lh2 / 2, 0);
                        swz.dispatchTouchEvent(ze);
                        ze.recycle();
                        for (int k = 1; k <= 10; k++) {
                            ze = android.view.MotionEvent.obtain(zt, zt + 200 + k * 16, android.view.MotionEvent.ACTION_MOVE,
                                    Ui.dp(ctx, 120), lh2 / 2 - k * 70, 0);
                            swz.dispatchTouchEvent(ze);
                            ze.recycle();
                        }
                        ze = android.view.MotionEvent.obtain(zt, zt + 420, android.view.MotionEvent.ACTION_UP,
                                Ui.dp(ctx, 120), lh2 / 2 - 700, 0);
                        swz.dispatchTouchEvent(ze);
                        ze.recycle();
                        z3 = sz.scrollYForTest() != zy0;

                        /* 钳制与行号槽跟缩（直驱同一套 clamp/quant/套用路径） */
                        sz.zoomByForTest(9f);
                        z4 = sz.fontSizeSpForTest() == ScriptEdit.ZOOM_MAX;
                        sz.zoomByForTest(0.001f);
                        z5 = sz.fontSizeSpForTest() == ScriptEdit.ZOOM_MIN;
                        sz.zoomByForTest(ScriptEdit.ZOOM_MAX / ScriptEdit.ZOOM_MIN);
                        swz.measure(View.MeasureSpec.makeMeasureSpec(lw2, View.MeasureSpec.EXACTLY),
                                View.MeasureSpec.makeMeasureSpec(lh2, View.MeasureSpec.EXACTLY));
                        swz.layout(0, 0, lw2, lh2);
                        z6 = sz.gutterWidthForTest() > zw1;

                        /* 渲染件（19.5sp 放大态）+ 记忆（真写路径：落盘 → 新实例读回） */
                        sz.setTextForTest("#模式 脚本\n如果 在区域(\"c1\", tux, tuy) {\n    等待 100\n}\n");
                        sz.refreshNowForTest();
                        sz.zoomByForTest(19.5f / ScriptEdit.ZOOM_MAX);
                        swz.measure(View.MeasureSpec.makeMeasureSpec(lw2, View.MeasureSpec.EXACTLY),
                                View.MeasureSpec.makeMeasureSpec(lh2, View.MeasureSpec.EXACTLY));
                        swz.layout(0, 0, lw2, lh2);
                        renderView(ctx, swz, "edit_script_zoom.png");
                        List<String> sv2 = new ArrayList<String>();
                        assertOverflow(swz, "脚本缩放", sv2, ctx);
                        ScriptEdit.zoomPersistSuppressedForTest = false;
                        sz.zoomByForTest(15f / 19.5f);
                        ScriptEdit sz2 = ScriptEdit.build(ctx, "", "", null);
                        z7 = sz2.fontSizeSpForTest() == 15f;
                        zk = z1 && z2 && z3 && z4 && z5 && z6 && z7 && sv2.isEmpty();
                    } finally {
                        ScriptEdit.zoomPersistSuppressedForTest = false;
                        zPref.edit().putFloat(ScriptEdit.PREF_KEY, zOld).apply();   /* 复原用户设置 */
                    }
                    rec(out, logFile, (ok1 && t1 && t2 && t3 && t4 && hk && zk && sv1.isEmpty() ? "[OK] " : "[FAIL] ")
                            + "T6 脚本编辑预览：edit_script.png + edit_script_err.png + edit_script_long.png + edit_script_hint.png + edit_script_zoom.png"
                            + "（打开路由=" + ok1 + " 高亮/辅助条/行号槽=" + t1 + " 错误行=" + t2 + " 滚动=" + t3
                            + " 拖拽滚动=" + t4 + " 提示条=" + hk
                            + " 缩放=" + zk + "（13→" + zgSeen + " 读数=" + z1 + "/" + z2 + " 单指=" + z3
                            + " 钳制=" + (z4 && z5) + " 行号=" + z6 + " 记忆=" + z7 + "）"
                            + (sv1.isEmpty() ? " · 溢出核 无" : " · 溢出 " + sv1.get(0)) + "）");
                    OpsV8.baseOverride = null;
                    Su.run("rm -rf '" + scr + "'");
                } catch (Throwable t) {
                    rec(out, logFile, "[FAIL] T6 脚本编辑预览异常: " + t);
                    OpsV8.baseOverride = null;
                }
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
            boolean hscroll = g instanceof android.widget.HorizontalScrollView;   // v35：横滚容器内容超宽属预期
            for (int i = 0; i < g.getChildCount(); i++) {
                View c = g.getChildAt(i);
                if (c.getVisibility() == View.GONE) continue;
                if (!hscroll && c.getRight() > pw + 4) viol.add(tag + " 横向溢出 " + describe(c) + " right=" + c.getRight() + " > " + pw);
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

    /** 深度查找文本完全等于 needle 的 TextView（渲染断言用；找不到 = null）。 */
    private static TextView findText(View v, String needle) {
        if (v instanceof TextView && needle.contentEquals(((TextView) v).getText())) return (TextView) v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                TextView t = findText(g.getChildAt(i), needle);
                if (t != null) return t;
            }
        }
        return null;
    }

    /** 同 findText，但取最后一个匹配（v12.4 块尾核：顶层「循环结束」在文档序末位）。 */
    private static TextView findTextLast(View v, String needle) {
        TextView r = null;
        if (v instanceof TextView && needle.contentEquals(((TextView) v).getText())) r = (TextView) v;
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                TextView t = findTextLast(g.getChildAt(i), needle);
                if (t != null) r = t;
            }
        }
        return r;
    }

    /** 视图在渲染根内的绝对左缘（沿 parent 链累加 getLeft）。 */
    private static int absLeft(View v) {
        int x = 0;
        View cur = v;
        while (cur != null) {
            x += cur.getLeft();
            android.view.ViewParent p = cur.getParent();
            cur = (p instanceof View) ? (View) p : null;
        }
        return x;
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

    /** v33：精确文本计数（「＋」行按钮核用；全树遍历，Text 完全相等才计）。 */
    private static int textCount(View v, String exact) {
        int n = 0;
        if (v instanceof TextView) {
            CharSequence t = ((TextView) v).getText();
            if (t != null && exact.contentEquals(t)) n++;
        }
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) n += textCount(g.getChildAt(i), exact);
        }
        return n;
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

                /* v22 画布自动写盘：改动 → 防抖(0.9s) → 自动落盘 + 清脏（scratch 重定向；本用例定点放开抑制） */
                String oldRO4 = RegionDraw.readOverride, oldPO4 = RegionDraw.pathOverride;
                boolean awOk = false, awLanded = false, awClean = false, awAttach = false;
                String awMsg = "";
                try {
                    java.io.File zfile = new java.io.File(scratch, "regions-auto.conf");
                    String zinit = "#vtouch-regions v2\n#frame 0 480 900\nregion r1 0 100 200 300 400 1\nbind r1 - down\nkind r1 0\n";
                    java.io.FileOutputStream zf = new java.io.FileOutputStream(zfile);
                    zf.write(zinit.getBytes("UTF-8"));
                    zf.close();
                    RegionDraw.readOverride = zfile.getAbsolutePath();
                    RegionDraw.pathOverride = zfile.getAbsolutePath();
                    byte[] rgbaZ = new byte[FW * FH * 4];
                    for (int i = 0; i < FW * FH; i++) { rgbaZ[i * 4 + 3] = (byte) 255; }
                    Capture.Shot shotZ = new Capture.Shot();
                    shotZ.w = FW; shotZ.h = FH; shotZ.rot = 0; shotZ.rgba = rgbaZ;
                    FrameStore.setDir(scratch);
                    FrameStore.save(shotZ);
                    final CountDownLatch lz = new CountDownLatch(1);
                    final VisualPage[] pvZ = new VisualPage[1];
                    mainSync(new Runnable() {
                        public void run() {
                            VisualPage.Session ses = new VisualPage.Session();
                            ses.mode = VisualPage.MODE_REGIONS;
                            VisualPage p = new VisualPage(ctx, ses, null);
                            p.attach(new Runnable() { public void run() { lz.countDown(); } });
                            pvZ[0] = p;
                        }
                    });
                    awAttach = lz.await(10, TimeUnit.SECONDS);
                    VisualPage.autoWriteSuppressedForTest = false;      /* 放开：验证自动调度链路 */
                    mainSync(new Runnable() {
                        public void run() {
                            pvZ[0].selectRegionForTest(0);
                            pvZ[0].typeNumForTest(0, 111);                 /* 走真实数值编辑路径 → markRegionsDirty */
                        }
                    });
                    for (int k = 0; k < 20 && !awLanded; k++) {            /* 防抖 0.9s + 写盘 → 轮询 ≤5s */
                        Thread.sleep(250);
                        byte[] zb;
                        try {
                            zb = new byte[(int) zfile.length()];
                            java.io.FileInputStream zfi = new java.io.FileInputStream(zfile);
                            zfi.read(zb); zfi.close();
                        } catch (Throwable igz) { continue; }
                        awLanded = new String(zb, "UTF-8").contains("region r1 0 111 200 300 400 1");
                    }
                    final boolean[] cdz = new boolean[1];
                    for (int k = 0; k < 12; k++) {          /* 内容落盘后还需等写收尾（镜像/记账→清脏）——忙时窗口宽，轮询等待 */
                        mainSync(new Runnable() { public void run() { cdz[0] = !pvZ[0].saveDirtyForTest(); } });
                        if (cdz[0]) break;
                        Thread.sleep(250);
                    }
                    awClean = cdz[0];
                    awOk = awAttach && awLanded && awClean;
                    awMsg = "落盘=" + awLanded + " 清脏=" + awClean;
                } catch (Throwable tz) {
                    awMsg = "EX:" + tz;
                } finally {
                    VisualPage.autoWriteSuppressedForTest = true;
                    RegionDraw.readOverride = oldRO4;
                    RegionDraw.pathOverride = oldPO4;
                }
                rec(out, logFile, (awOk ? "[OK] " : "[FAIL] ")
                        + "画布自动写盘 v22（数值编辑 → 防抖自动落盘 + 清脏） " + awMsg);
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