package com.vtouch.editor;

import android.content.Context;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * 跨方案复制（区域 / 操作 → 其他方案）。
 *
 * 口径（2026-10-09 拍板）：
 *  - 依赖自动携带：复制操作带上它引用的区域（门控 / 区域判断 / 开关判断 / 区域限定），
 *    复制区域带上它绑定的操作；递归取闭包（防环）。目标已有同名依赖 → 不复制、引用目标现有的。
 *  - 本体与目标同名（冲突）→ 弹窗二选一：改名复制（「原名-2」取首个空闲；随行集合内指向
 *    本体的引用自动改写）/ 覆盖（替换目标同名项、保留原名）。
 *  - 找图模板 / 点集是运行时全局共享（templates/），不随方案走；基帧（#frame）差异只提示不换算。
 *
 * 两层：
 *  - inspect / render：纯逻辑（吃吐文本、无 IO，测试台直接可测）；render 消费 Inspect 一次；
 *  - copyFlow：UI 流程（列表页 [复制到…] 整条链：选方案 → 预检 → 冲突/基帧弹窗 → 写盘 → toast）。
 */
public final class CrossCopy {

    private CrossCopy() { }

    // ================================================================ 纯逻辑

    /** 预检结果（inspect 返回）。 */
    public static final class Inspect {
        public String err;                  // 非 null = 失败（源缺项 / 解析失败）
        public boolean isRegion;            // true=复制区域，false=复制操作
        public String name;                 // 本体原名
        public boolean conflict;            // 目标已有同名本体
        public String renamePreview;        // 冲突时改名预览（「原名-2」取首个空闲）
        public final List<String> carries = new ArrayList<String>();   // 将一并复制："区域 xx" / "操作 xx"
        public String frameWarn;            // 基帧不同提示（null = 无）
        // 内部（render 用）
        Regions.File srcRF, dstRF;
        Ops.File srcOF, dstOF;
        Regions.Region srcReg;
        Ops.Op srcOp;
        final List<Regions.Region> carryRegions = new ArrayList<Regions.Region>();
        final List<Ops.Op> carryOps = new ArrayList<Ops.Op>();
    }

    /** 生成结果（render 返回）：两份新文本 + 本体最终名。 */
    public static final class Rendered {
        public String err;
        public String newDstRegions;
        public String newDstOps;
        public String appliedName;
    }

    /** 预检：解析四份文本、判冲突、收依赖闭包、算改名预览与基帧提示。 */
    public static Inspect inspect(boolean isRegion, String name,
                                  String srcR, String srcO, String dstR, String dstO) {
        Inspect ins = new Inspect();
        ins.isRegion = isRegion;
        ins.name = name;
        if (srcR == null || srcO == null) { ins.err = "读取当前方案失败（root 不可用？）"; return ins; }
        int keepRot = Regions.lastFrameRot, keepW = Regions.lastFrameW, keepH = Regions.lastFrameH;
        try {
            ins.srcRF = Regions.File.parse(withHeader(srcR, "#vtouch-regions v2"));
            keepRot = Regions.lastFrameRot; keepW = Regions.lastFrameW; keepH = Regions.lastFrameH;
            ins.srcOF = Ops.File.parse(withHeader(srcO, "#vtouch-ops v5"));
            ins.dstRF = Regions.File.parse(withHeader(dstR, "#vtouch-regions v2"));
            ins.dstOF = Ops.File.parse(withHeader(dstO, "#vtouch-ops v5"));
        } catch (Throwable t) {
            ins.err = "方案文件解析失败：" + t.getMessage();
            return ins;
        } finally {
            /* 目标不是 live：lastFrame 恢复成 live（源）口径，避免污染操作页坐标校验 */
            Regions.lastFrameRot = keepRot;
            Regions.lastFrameW = keepW;
            Regions.lastFrameH = keepH;
        }
        if (isRegion) {
            ins.srcReg = ins.srcRF.find(name);
            if (ins.srcReg == null) { ins.err = "当前方案里没有区域「" + name + "」（可能是未保存修改）"; return ins; }
            ins.conflict = ins.dstRF.find(name) != null;
        } else {
            ins.srcOp = ins.srcOF.find(name);
            if (ins.srcOp == null) { ins.err = "当前方案里没有操作「" + name + "」（可能是未保存修改）"; return ins; }
            ins.conflict = ins.dstOF.find(name) != null;
        }
        /* 依赖闭包（BFS：区域 → 绑定操作；操作 → 引用区域） */
        Set<String> seenR = new HashSet<String>(), seenO = new HashSet<String>();
        List<String> qR = new ArrayList<String>(), qO = new ArrayList<String>();
        if (isRegion) { seenR.add(name); qR.add(name); } else { seenO.add(name); qO.add(name); }
        while (!qR.isEmpty() || !qO.isEmpty()) {
            if (!qR.isEmpty()) {
                Regions.Region r = ins.srcRF.find(qR.remove(0));
                String on = r == null ? null : r.bindOp;
                if (on != null && !on.isEmpty() && !"-".equals(on) && !seenO.contains(on)) {
                    seenO.add(on);
                    Ops.Op dep = ins.srcOF.find(on);
                    if (dep != null && ins.dstOF.find(on) == null) {   // 悬空 / 目标已有 → 不携带
                        ins.carryOps.add(dep);
                        ins.carries.add("操作 " + on);
                        qO.add(on);
                    }
                }
            }
            if (!qO.isEmpty()) {
                Ops.Op o = ins.srcOF.find(qO.remove(0));
                if (o == null) continue;
                for (String rn : opRegionRefs(o)) {
                    if (seenR.contains(rn)) continue;
                    seenR.add(rn);
                    Regions.Region dep = ins.srcRF.find(rn);
                    if (dep != null && ins.dstRF.find(rn) == null) {
                        ins.carryRegions.add(dep);
                        ins.carries.add("区域 " + rn);
                        qR.add(rn);
                    }
                }
            }
        }
        if (ins.conflict) {
            Set<String> used = new HashSet<String>();
            if (isRegion) {
                for (Regions.Region r : ins.dstRF.regions) used.add(r.id);
                for (Regions.Region r : ins.carryRegions) used.add(r.id);
            } else {
                for (Ops.Op o : ins.dstOF.ops) used.add(o.name);
                for (Ops.Op o : ins.carryOps) used.add(o.name);
            }
            ins.renamePreview = freeName(name, used);
            if (ins.renamePreview == null) { ins.err = "找不到可用新名（「原名-2/-3…」都被占用）"; return ins; }
        }
        ins.frameWarn = frameWarnFor(ins.srcRF, ins.dstRF);
        return ins;
    }

    /** 生成目标方案的新文本。renameMode=true 改名复制；false 覆盖（无冲突时即直插）。消费 Inspect 一次。 */
    public static Rendered render(Inspect ins, boolean renameMode) {
        Rendered rd = new Rendered();
        if (ins == null) { rd.err = "无预检结果"; return rd; }
        if (ins.err != null) { rd.err = ins.err; return rd; }
        try {
            boolean renamed = ins.conflict && renameMode;
            String applied = renamed ? ins.renamePreview : ins.name;
            rd.appliedName = applied;

            /* 区域侧 */
            if (ins.isRegion) {
                Regions.Region copy = ins.srcReg.copy();
                if (renamed) { copy.id = applied; ins.dstRF.regions.add(copy); }
                else replaceOrAddRegion(ins.dstRF, ins.name, copy);
            }
            for (Regions.Region r : ins.carryRegions) {
                Regions.Region copy = r.copy();
                if (!ins.isRegion && renamed && ins.name.equals(copy.bindOp)) copy.bindOp = applied;
                ins.dstRF.regions.add(copy);
            }

            /* 操作侧 */
            if (!ins.isRegion) {
                Ops.Op copy = ins.srcOp.copy();
                if (renamed) { copy.name = applied; ins.dstOF.ops.add(copy); }
                else replaceOrAddOp(ins.dstOF, ins.name, copy);
            }
            for (Ops.Op o : ins.carryOps) {
                Ops.Op copy = o.copy();
                if (ins.isRegion && renamed) remapOpRegionRefs(copy, ins.name, applied);
                ins.dstOF.ops.add(copy);
            }

            rd.newDstRegions = ins.dstRF.serialize();
            rd.newDstOps = ins.dstOF.serialize();
        } catch (Throwable t) {
            rd.err = "生成失败：" + t;
        }
        return rd;
    }

    /** 操作引用的区域名（门控 + 各步：区域/开关判断 ref、找图/找色区域限定 expr）。 */
    static List<String> opRegionRefs(Ops.Op o) {
        List<String> out = new ArrayList<String>();
        if (o.gate != null && !o.gate.isEmpty() && !"-".equals(o.gate)) out.add(o.gate);
        for (Ops.Step s : o.steps) {
            if (s.raw != null) continue;
            if ((s.type == Ops.T_COND_REGION || s.type == Ops.T_COND_TOGGLE)
                    && s.ref != null && !s.ref.isEmpty()) out.add(s.ref);
            else if ((s.type == Ops.T_FINDIMAGE || s.type == Ops.T_FINDCOLOR)
                    && s.expr != null && !s.expr.isEmpty()) out.add(s.expr);
        }
        return out;
    }

    /** 把操作里对区域 from 的引用改成 to（跟随改名复制，保组内自洽）。 */
    static void remapOpRegionRefs(Ops.Op o, String from, String to) {
        if (from.equals(o.gate)) o.gate = to;
        for (Ops.Step s : o.steps) {
            if (s.raw != null) continue;
            if ((s.type == Ops.T_COND_REGION || s.type == Ops.T_COND_TOGGLE) && from.equals(s.ref)) s.ref = to;
            else if ((s.type == Ops.T_FINDIMAGE || s.type == Ops.T_FINDCOLOR) && from.equals(s.expr)) s.expr = to;
        }
    }

    /** 覆盖时替换目标同名槽位（保持顺序）；找不着就追加（无冲突直插路径）。 */
    static void replaceOrAddRegion(Regions.File f, String origName, Regions.Region item) {
        for (int i = 0; i < f.regions.size(); i++) {
            if (f.regions.get(i).id.equals(origName)) { f.regions.set(i, item); return; }
        }
        f.regions.add(item);
    }

    static void replaceOrAddOp(Ops.File f, String origName, Ops.Op item) {
        for (int i = 0; i < f.ops.size(); i++) {
            if (f.ops.get(i).name.equals(origName)) { f.ops.set(i, item); return; }
        }
        f.ops.add(item);
    }

    /** 首个空闲的「原名-2/-3…」（可见字符 ≤15，超长按码点截断前缀）。 */
    static String freeName(String base, Set<String> used) {
        for (int i = 2; i < 1000; i++) {
            String suf = "-" + i;
            String cand = trimCp(base, 15 - suf.length()) + suf;
            if (!used.contains(cand) && Regions.idOk(cand)) return cand;
        }
        return null;
    }

    /** 按码点截断到 ≤maxCp 个可见字符。 */
    static String trimCp(String s, int maxCp) {
        if (s == null) return "";
        int n = 0;
        for (int i = 0; i < s.length(); ) {
            int c = s.codePointAt(i);
            if (n >= maxCp) return s.substring(0, i);
            i += Character.charCount(c);
            n++;
        }
        return s;
    }

    /** 基帧差异提示：方向不同、或两边都明确标注且尺寸不同 → 提醒（坐标按原样复制）。 */
    static String frameWarnFor(Regions.File s, Regions.File d) {
        boolean rotDiff = (s.frameRot & 3) != (d.frameRot & 3);
        boolean sizeDiff = s.frameW > 0 && s.frameH > 0 && d.frameW > 0 && d.frameH > 0
                && (s.frameW != d.frameW || s.frameH != d.frameH);
        if (!rotDiff && !sizeDiff) return null;
        return "注意：两方案基帧不同（当前 " + frameDesc(s) + "，目标 " + frameDesc(d)
                + "），区域坐标按原样复制、可能位置不对。";
    }

    static String frameDesc(Regions.File f) {
        String ori = (f.frameRot & 1) == 0 ? "竖屏" : "横屏";
        return (f.frameW <= 0 || f.frameH <= 0) ? (ori + "（未标注尺寸）") : (ori + " " + f.frameW + "×" + f.frameH);
    }

    /** 空 / 缺文件兜底成带版本行的空表。 */
    static String withHeader(String t, String header) {
        return (t == null || t.trim().isEmpty()) ? (header + "\n") : t;
    }

    static String join(List<String> l) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < l.size(); i++) { if (i > 0) sb.append('、'); sb.append(l.get(i)); }
        return sb.toString();
    }

    // ================================================================ UI 流程

    /** 列表页 [复制到…] 入口：列方案 → 选目标 → 预检 → 冲突/基帧弹窗或直复制 → toast。 */
    public static void copyFlow(final Context act, final boolean isRegion, final String name, final boolean dirty) {
        Ui.bg(new Runnable() {
            public void run() {
                final List<String> all = Schemes.list();
                final String cur = Schemes.current();
                final java.util.Map<String, String> counts = Schemes.counts();
                final List<String> names = new ArrayList<String>();
                final List<String> labels = new ArrayList<String>();
                for (String s : all) {
                    if (s.equals(cur)) continue;               /* 只列"其他"方案（复制到当前无意义） */
                    names.add(s);
                    String c = counts.get(s);
                    labels.add(c == null ? s : s + " — " + c);
                }
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (names.isEmpty()) {
                            Ui.toast("没有其他方案可复制（先在「方案」页新建 / 另存）");
                            return;
                        }
                        if (dirty) Ui.toast("提示：将按已保存版本复制（当前页有未保存修改）");
                        Forms.pickDialog(act, "复制「" + name + "」到哪个方案？", labels, -1, new Forms.Pick() {
                            public void onPick(int i) { preflight(act, isRegion, name, names.get(i)); }
                        });
                    }
                });
            }
        }, null);
    }

    static String dstPath(String scheme, boolean regions) {
        return Schemes.DIR + "/" + scheme + (regions ? "/regions.conf" : "/ops.conf");
    }

    /** 后台读四份文本做预检；回主线程弹窗或直复制。 */
    static void preflight(final Context act, final boolean isRegion, final String name, final String target) {
        Ui.bg(new Runnable() {
            public void run() {
                final String srcR = Su.read(Regions.PATH);
                final String srcO = Su.read(Ops.PATH);
                final String dstR = Su.read(dstPath(target, true));
                final String dstO = Su.read(dstPath(target, false));
                final Inspect ins = inspect(isRegion, name, srcR, srcO, dstR, dstO);
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (ins.err != null) { Ui.toast(ins.err); return; }
                        if (ins.conflict) conflictDialog(act, ins, target, dstR, dstO);
                        else if (ins.frameWarn != null) frameDialog(act, ins, target, dstR, dstO);
                        else execute(act, ins, false, target, dstR, dstO);
                    }
                });
            }
        }, null);
    }

    /** 冲突弹窗：改名复制（主）/ 覆盖（警示）/ 取消。 */
    static void conflictDialog(final Context act, final Inspect ins, final String target,
                               final String dR, final String dO) {
        Ui.Dlg d = buildConflictDialog(act, ins, target, new Runnable() {
            public void run() { execute(act, ins, false, target, dR, dO); }
        }, new Runnable() {
            public void run() { execute(act, ins, true, target, dR, dO); }
        });
        d.show();
    }

    /** 构建冲突弹窗（不显示；供离屏渲染预览与执行复用）。onOverwrite/onRename 为 null 时按钮只关闭。 */
    static Ui.Dlg buildConflictDialog(final Context act, final Inspect ins, final String target,
                                      final Runnable onOverwrite, final Runnable onRename) {
        String kind = ins.isRegion ? "区域" : "操作";
        final Ui.Dlg d = Ui.dialog(act, "复制「" + ins.name + "」到「" + target + "」");
        d.body.addView(Ui.dlgMessage(act, "目标已有同名" + kind + "「" + ins.name + "」。"));
        d.body.addView(Ui.dlgMessage(act, "· 改名复制：保留目标原有，新副本「" + ins.renamePreview + "」"));
        d.body.addView(Ui.dlgMessage(act, "· 覆盖：替换目标那一个（名字不变）"));
        if (!ins.carries.isEmpty()) d.body.addView(Ui.dlgMessage(act, "将一并复制：" + join(ins.carries)));
        if (ins.frameWarn != null) d.body.addView(Ui.dlgMessage(act, ins.frameWarn));
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { d.dismiss(); }
        });
        TextView ov = Ui.btnSmallTonal(act, "覆盖", Ui.WARN);
        ov.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) {
                d.dismiss();
                if (onOverwrite != null) onOverwrite.run();
            }
        });
        TextView rn = Ui.btnSmallPrimary(act, "改名复制");
        rn.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) {
                d.dismiss();
                if (onRename != null) onRename.run();
            }
        });
        d.actions.addView(cancel);
        d.actions.addView(ov);
        d.actions.addView(rn);
        return d;
    }

    /** 基帧差异确认（无冲突）：继续复制 / 取消。 */
    static void frameDialog(final Context act, final Inspect ins, final String target,
                            final String dR, final String dO) {
        final Ui.Dlg d = Ui.dialog(act, "复制「" + ins.name + "」到「" + target + "」？");
        d.body.addView(Ui.dlgMessage(act, ins.frameWarn));
        if (!ins.carries.isEmpty()) d.body.addView(Ui.dlgMessage(act, "将一并复制：" + join(ins.carries)));
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { d.dismiss(); }
        });
        TextView ok = Ui.btnSmallPrimary(act, "继续复制");
        ok.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { d.dismiss(); execute(act, ins, false, target, dR, dO); }
        });
        d.actions.addView(cancel);
        d.actions.addView(ok);
        d.show();
    }

    /** 生成新文本并写目标方案（备份 + 原子写，仅写有变化的文件）；完成后 toast。 */
    static void execute(final Context act, final Inspect ins, final boolean renameMode,
                        final String target, final String dR, final String dO) {
        final Rendered rd = render(ins, renameMode);
        if (rd.err != null) { Ui.toast(rd.err); return; }
        final String kind = ins.isRegion ? "区域" : "操作";
        Ui.bg(new Runnable() {
            public void run() {
                String err = null;
                if (!rd.newDstRegions.equals(dR == null ? "" : dR)) {
                    Su.backup(dstPath(target, true));
                    Su.R r = Su.writeAtomic(dstPath(target, true), rd.newDstRegions.getBytes(StandardCharsets.UTF_8));
                    if (!r.ok()) err = "区域文件写入失败：" + r.trimmed();
                }
                if (err == null && !rd.newDstOps.equals(dO == null ? "" : dO)) {
                    Su.backup(dstPath(target, false));
                    Su.R r = Su.writeAtomic(dstPath(target, false), rd.newDstOps.getBytes(StandardCharsets.UTF_8));
                    if (!r.ok()) err = "操作文件写入失败：" + r.trimmed();
                }
                final String e = err;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (e != null) { Ui.toast("复制失败：" + e); return; }
                        String extra = ins.carries.isEmpty() ? "" : "；一并带过去：" + join(ins.carries);
                        Ui.toast("已复制" + kind + "「" + rd.appliedName + "」到方案「" + target + "」"
                                + extra + "（切换方案后生效）");
                    }
                });
            }
        }, null);
    }
}
