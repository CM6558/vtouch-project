package com.vtouch.editor;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * 操作页：ops.conf 编辑。
 * 列表 ↔ 操作编辑（门控/自动关/步骤流） ↔ 步骤编辑（全屏表单）。
 * 门控/开关判断口径（对齐核心）：引用的区域必须「存在 + 开关型」，否则核心按 门控拦截 / 非开关型 处理。
 * v12.2：条件块分支插入——块头 [＋] 落「成立分支」末尾（否则之前）；「否则」分隔行 [＋] 落否则分支末尾。
 * v26：① 切页/刷新保留编辑会话（busy() 供 Shell 判断）；② 移动全覆盖——块头=整块一起移（含子条件/块体/否则/块尾）、
 * 子条件行=组内换序、落点结构预检（损坏回滚）；③ 添加/移动后自动滚动定位到目标行。
 * v31：① 卡片视图收起代码块（块头 ▼/▸；收起集与流程总览共用）；② 步骤复制——行 [复制]（块头=整块）、底栏 [粘贴（N 步）]
 * →「粘贴到…」合法落点对话框（试用插入 + 结构预检；原样深拷贝 + 轻预检提示）；③ 连选——长按步骤行进入，点行定范围，
 * [复制所选]；块边界自动补全（整块/子条件链随头走）。
 */
public class OpsPage {
    private final Context act;
    private LinearLayout bar, container, bottomBar;
    private ScrollView listSc;             // v26：列表滚动容器（添加/移动后定位用）
    private int stepReturnY = -1;          // v27：进入步骤编辑前的滚动位置（返回时恢复原处，不回顶）
    public int restoreYRequest = -1;       // v27：待恢复的滚动请求（post 与测试直驱共用）
    private int restoreTries = 0;          // v27：未量测重投计数
    private TextView statusLine;
    private Ops.File file;
    private List<String> regionIds = new ArrayList<String>();    // 全部区域（区域判断用）
    private List<String> switchIds = new ArrayList<String>();    // 仅开关型（门控 / 开关判断用）
    private List<String> tmplNames = new ArrayList<String>();    // 模板（.tmpl）名字（找图用）
    private List<String> ptsNames = new ArrayList<String>();     // 点集（.pts）名字（找色多点用）
    private List<Regions.Region> regionsAll = new ArrayList<Regions.Region>();   // 全量区域（区域限定换算用）
    private boolean dirty = false;
    private Ops.Op editing = null;         // 编辑中的副本
    private int editIndex = -1;
    private Ops.Step stepEditing = null;   // 步骤编辑中的副本（null = 不在步骤编辑）
    private StepEdit stepEdit = null;      // v32：当前步骤编辑页构建器（[保存] 与测试直驱共用；renderStepEditor 重建）
    private int stepIndex = -1;            // -1 = 新增步骤
    private int insertBefore = -1;         // v12：块内插入的目标位置（-1 = 末尾）
    private String insertCtx = "";         // v12.2：插入落点语境（成立分支 / 否则分支 / 循环体；进插入提示）
    private int presetType = 0;            // v12.1：弹层选定的类型（用后清零 = 点按）
    private boolean overview = false;      // 步骤流：总览模式（紧凑时间线）
    private final java.util.Set<Ops.Step> foldedBlocks =     // v12.3：已收起的块（按步骤对象身份；编辑后不串位）；v31：卡片视图与总览共用
            java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<Ops.Step, Boolean>());

    // v35（T6 双视图）：编辑会话视图模式（块/脚本）+ 脚本会话态
    private String editMode = "块";                       // 当前会话视图（保存时定锁；缺省由 #模式 侧线/可投影性推导）
    private ScriptEdit scriptEdit = null;                 // 脚本编辑页（会话内建一次，保留光标/滚动）；null = 未建
    private String scriptSeed = null;                     // 脚本会话初始文本（切块/保存读 scriptEdit.text()）
    private LinearLayout editorHost = null;               // 脚本编辑宿主（build 挂载；非脚本模式 GONE）
    /** v35：列表卡「脚本 N 行」——progs/*.vts 行数（refresh 后台填）。 */
    public final java.util.HashMap<String, Integer> opLines = new java.util.HashMap<String, Integer>();

    // v31：连选（长按步骤行进入；selA = 锚点行、selB = 另一端行——均为原始下标；显示/复制范围 = 归一后）
    private int selA = -1, selB = -1;
    private static boolean selHintShown = false;      // 连选提示只弹一次

    /** v26：定位——idx → 行视图（卡片/总览共用；render() 重填），「添加/移动后滚动到目标行」用。 */
    private final java.util.HashMap<Integer, View> focusRows = new java.util.HashMap<Integer, View>();
    /** v26：待执行定位请求（-1 = 无；设置后于下一帧滚动执行；测试直驱可读）。 */
    public int focusIdxRequest = -1;
    /** v26：定位重试计数（post 回调可能早于本帧布局 → 未量好时重投，上限 10）。 */
    private int focusTries = 0;

    public OpsPage(Context a) { act = a; }

    public View build() {
        LinearLayout root = Ui.col(act);
        bar = Ui.row(act);
        bar.setPadding(Ui.dp(act, 16), Ui.dp(act, 10), Ui.dp(act, 8), Ui.dp(act, 6));
        statusLine = Ui.mono(act, "", 12, Ui.DIM);
        statusLine.setSingleLine(true);
        statusLine.setEllipsize(android.text.TextUtils.TruncateAt.END);
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(bar);
        listSc = new ScrollView(act);
        container = Ui.col(act);
        container.setPadding(Ui.dp(act, 8), Ui.dp(act, 4), Ui.dp(act, 8), Ui.dp(act, 16));
        listSc.addView(container);
        root.addView(listSc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        // v35：脚本编辑宿主（与列表互斥；承载 ScriptEdit.root；非脚本模式 GONE）
        editorHost = Ui.col(act);
        editorHost.setVisibility(View.GONE);
        root.addView(editorHost, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        // 底部常驻操作条（编辑模式下显示「＋ 添加步骤」；不随列表滚动）
        bottomBar = Ui.col(act);
        bottomBar.setVisibility(View.GONE);
        root.addView(bottomBar);
        return root;
    }

    public void refresh() {
        Ui.bg(new Runnable() {
            public void run() {
                final String text = Su.read(Ops.PATH);
                final List<String> ids = new ArrayList<String>();
                final List<String> sw = new ArrayList<String>();
                final List<Regions.Region> rAll = new ArrayList<Regions.Region>();
                String rt = Su.read(Regions.PATH);
                if (rt != null) {
                    try {
                        Regions.File rf = Regions.File.parse(rt);
                        for (Regions.Region r : rf.regions) {
                            ids.add(r.id);
                            if (r.kind == 1) sw.add(r.id);
                            rAll.add(r);
                        }
                    } catch (Exception e) { /* 忽略 */ }
                }
                final List<String> tmpls = new ArrayList<String>();
                final List<String> ptss = new ArrayList<String>();
                Su.R tl = Su.run("ls " + Ops.TMPL_DIR + " 2>/dev/null");
                if (tl.ok() && tl.out != null) {
                    for (String ln : tl.out.split("\n")) {
                        String fn = ln.trim();
                        if (fn.endsWith(".tmpl")) tmpls.add(fn.substring(0, fn.length() - 5));
                        else if (fn.endsWith(".pts")) ptss.add(fn.substring(0, fn.length() - 4));
                    }
                }
                final java.util.HashMap<String, Integer> linesMap = new java.util.HashMap<String, Integer>();
                Su.R wl = Su.run("wc -l " + OpsV8.base() + "/progs/*.vts 2>/dev/null");
                if (wl.ok() && wl.out != null) {
                    for (String ln : wl.out.split("\n")) {
                        String t = ln.trim();
                        int sp = t.indexOf(' ');
                        if (sp <= 0) continue;
                        String num = t.substring(0, sp).trim();
                        String path = t.substring(sp).trim();
                        int sl = path.lastIndexOf('/');
                        String bn = sl >= 0 ? path.substring(sl + 1) : path;
                        if (!bn.endsWith(".vts")) continue;                 // 含 wc 的 total 行
                        try { linesMap.put(bn.substring(0, bn.length() - 4), Integer.valueOf(Integer.parseInt(num))); }
                        catch (Exception e) { /* 忽略 */ }
                    }
                }
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        opLines.clear();
                        opLines.putAll(linesMap);
                        applyLoad(text, ids, sw, tmpls, ptss, rAll);
                    }
                });
            }
        }, null);
    }

    /** 内容装载（refresh 异步回调）。v26：**不清空编辑会话**——切页/刷新不打断正在编辑的内容
     *  （editing/stepEditing 保留，重渲染即可）；另以包内可见供 TestBench 直驱。 */
    void applyLoad(String text, List<String> ids, List<String> sw,
                           List<String> tmpls, List<String> ptss, List<Regions.Region> rAll) {
        regionIds = ids;
        switchIds = sw;
        regionsAll = rAll != null ? rAll : new ArrayList<Regions.Region>();
        tmplNames = tmpls != null ? tmpls : new ArrayList<String>();
        ptsNames = ptss != null ? ptss : new ArrayList<String>();
        if (text == null) {
            statusLine.setText("读取失败：ops.conf 不存在或 root 不可用");
            if (editing == null) container.removeAllViews(); else render();
            return;
        }
        try {
            file = Ops.File.parse(text);
            dirty = false;
            /* v26：原「editing = null; stepEditing = null」已删——编辑中刷新/切页保持现场 */
            render();
        } catch (Exception e) {
            statusLine.setText("解析失败：" + e.getMessage());
            container.removeAllViews();
        }
    }

    /** v26：是否有进行中的编辑会话（Shell 切页/刷新据此保留现场，不重载）。 */
    public boolean busy() { return editing != null; }

    private void render() {
        if (file == null) return;
        focusRows.clear();                       // v26：行视图表随渲染重建
        boolean scriptMode = editing != null && stepEditing == null && "脚本".equals(editMode);
        if (scriptMode) { renderScriptEditor(); return; }
        if (listSc != null) listSc.setVisibility(View.VISIBLE);
        if (editorHost != null && editorHost.getVisibility() != View.GONE) {
            editorHost.removeAllViews();
            editorHost.setVisibility(View.GONE);
        }
        if (editing == null) renderList();
        else if (stepEditing == null) renderEditor();
        else renderStepEditor();
    }

    /** 门控 / 开关判断引用的区域必须存在且为开关型；返回错误文案或 null。 */
    private String regionRefErr(Ops.Op o) {
        if (!"-".equals(o.gate) && !switchIds.contains(o.gate)) {
            return "操作「" + o.name + "」的门控「" + o.gate + "」不是开关型区域（核心会拦截此操作）";
        }
        for (int i = 0; i < o.steps.size(); i++) {
            Ops.Step s = o.steps.get(i);
            if (s.raw == null && s.type == Ops.T_COND_TOGGLE && !s.ref.isEmpty()
                    && !switchIds.contains(s.ref)) {
                return "操作「" + o.name + "」第 " + (i + 1) + " 步开关判断引用的「" + s.ref
                        + "」不是开关型区域";
            }
        }
        return null;
    }

    // ---------- 列表模式 ----------

    private void renderList() {
        bottomBar.setVisibility(View.GONE);
        bar.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView add = Ui.btnSmallPrimary(act, "＋ 新建操作");   // v32：页面唯一主操作 = 实心键
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { newOpDialog(); }
        });
        bar.addView(add);

        container.removeAllViews();
        statusLine.setText("共 " + file.ops.size() + " 个操作（上限 16）");   /* v22：提交即落盘，无待保存态 */
        if (file.ops.isEmpty()) {
            container.addView(Ui.emptyState(act, "还没有操作",
                    "操作由多个步骤组成：点击 / 滑动 / 等待 / 条件判断…",
                    "＋ 新建操作", new View.OnClickListener() {
                        public void onClick(View v) { newOpDialog(); }
                    }));
            return;
        }
        for (int i = 0; i < file.ops.size(); i++) {
            final int idx = i;
            final Ops.Op o = file.ops.get(i);
            LinearLayout card = Ui.card(act);
            card.setBackground(Ui.pressableStroked(act, Ui.CARD, Ui.CARD_LINE, 0xFF23272C, 8));
            card.setClickable(true);
            card.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { startEdit(idx); }
            });
            LinearLayout titleRow = Ui.row(act);
            titleRow.addView(Ui.monoBold(act, o.name, 15, Ui.TEXT),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView edit = Ui.btnFlat(act, "✎ 编辑", Ui.ACCENT);
            edit.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { startEdit(idx); }
            });
            TextView cp = Ui.btnFlat(act, "复制到…", Ui.DIM);
            cp.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { CrossCopy.copyFlow(act, false, o.name, dirty); }
            });
            TextView del = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
            del.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { deleteOp(idx); }
            });
            titleRow.addView(edit);
            titleRow.addView(cp);
            titleRow.addView(del);
            card.addView(titleRow);
            LinearLayout metaRow = Ui.row(act);
            metaRow.setPadding(0, Ui.dp(act, 2), 0, 0);
            if (o.progHash != null) {
                Integer lc = opLines.get(o.name);
                metaRow.addView(Ui.chip(act, lc == null ? "脚本操作" : ("脚本 " + lc.intValue() + " 行"), Ui.DIM));
            } else {
                metaRow.addView(Ui.chip(act, o.steps.size() + " 步", Ui.DIM));
            }
            if (!"-".equals(o.gate)) {
                metaRow.addView(Ui.chip(act, "门控 " + o.gate,
                        switchIds.contains(o.gate) ? Ui.ACCENT : Ui.WARN));
            }
            if (o.autooff == 1) metaRow.addView(Ui.chip(act, "跑完自动关", Ui.OK));
            card.addView(metaRow);
            container.addView(card);
        }
    }

    private void startEdit(int idx) {
        Ops.Op rec = file.ops.get(idx).copy();
        editing = rec;
        editIndex = idx;
        foldedBlocks.clear();               // v12.3：收起状态随编辑会话重置
        selA = -1; selB = -1;               // v31：连选随编辑会话重置
        editMode = "块";
        scriptSeed = null;
        scriptEdit = null;
        if (rec.progHash != null) {
            // v35（T6）：v8 操作——读脚本站源 → 视图 = #模式侧线 ??（可投影 ? 块 : 脚本）
            String src = OpsV8.readSrc(OpsV8.base(), rec.name);
            if (src == null) {
                editing = null;
                editIndex = -1;
                Ui.toast("读不到脚本站：progs/" + rec.name + ".vts 缺失");
                render();
                return;
            }
            scriptSeed = src;
            String mode = ScriptStore.modeOf(src);
            if (mode == null) mode = VtToSteps.project(src).err == null ? "块" : "脚本";
            if ("块".equals(mode)) {
                VtToSteps.R p = VtToSteps.project(src);
                if (p.err != null) mode = "脚本";     // 侧线说块但投影不过（外部改盘）→ 退回脚本
                else {
                    rec.steps.clear();
                    rec.steps.addAll(p.op.steps);
                }
            }
            editMode = mode;
        }
        render();
    }

    private void newOpDialog() {
        if (file.ops.size() >= 16) { Ui.toast("操作数已达上限 16"); return; }
        final EditText nameE = Ui.textField(act, "操作名（1..15 字，可用中文）");
        LinearLayout box = Ui.col(act);
        box.addView(Ui.label(act, "操作名"));
        box.addView(nameE);
        final Ui.Dlg dlg = Ui.dialog(act, "新建操作");
        dlg.body.addView(box);
        TextView cancelB = Ui.btnFlat(act, "取消", Ui.DIM);
        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { dlg.dismiss(); }
        });
        TextView okB = Ui.btnSmallPrimary(act, "创建");
        dlg.actions.addView(cancelB);
        dlg.actions.addView(okB);
        dlg.show();
        okB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String name = nameE.getText().toString().trim();
                String err = startNewDraft(name);
                if (err != null) { Ui.toast(err); return; }
                dlg.dismiss();
                Ui.toast("操作「" + name + "」：添加步骤后点「完成」创建");
            }
        });
    }

    /** 新建操作草稿：只校验名字（步数到 [完成] 再查——核心要求 1..512 步）→ 直接进编辑页。
     *  草稿不在列表里：[完成] 才创建、[取消] 丢弃。返回错误文案或 null（供 TestBench 直驱）。 */
    public String startNewDraft(String name) {
        Ops.Op draft = new Ops.Op();
        draft.name = name;
        String err = Ops.validateName(draft, file, null);
        if (err != null) return err;
        editing = draft;
        editIndex = -1;
        overview = false;
        editMode = "块";                     // v35：新草稿缺省块视图（会话内可 [转脚本]）
        scriptSeed = null;
        scriptEdit = null;
        foldedBlocks.clear();
        selA = -1; selB = -1;               // v31
        render();
        return null;
    }

    private void deleteOp(final int idx) {
        final Ops.Op o = file.ops.get(idx);
        final Ui.Dlg d = Ui.dialog(act, "删除操作 " + o.name + "？");
        d.body.addView(Ui.dlgMessage(act, "引用它的区域绑定 / 门控会变为悬空（核心按未命中处理）。"));
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView ok = Ui.btnSmallTonal(act, "删除", Ui.WARN);
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                file.ops.remove(idx);
                dirty = true;
                render();
                d.dismiss();
                java.util.List<String> rm = new java.util.ArrayList<String>();
                rm.add(o.name);                      // v35：v8 产物随删（镜像同步清扫）
                saveEx("已删除操作「" + o.name + "」", rm);   /* v22：删除即落盘 */
            }
        });
        d.actions.addView(cancel);
        d.actions.addView(ok);
        d.show();
    }

    // ---------- 操作编辑模式 ----------

    private void renderEditor() {
        final Ops.Op eo = editing;
        bar.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        statusLine.setText("编辑中：操作 " + eo.name + "（" + eo.steps.size() + " 步）");
        TextView cancel = Ui.btnSmall(act, "取消");
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                boolean draft = editIndex < 0;       // 草稿取消 = 丢弃新建（不在列表里）
                editing = null;
                render();
                if (draft) Ui.toast("已取消：未创建操作");
            }
        });
        TextView toScript = Ui.btnFlat(act, "转脚本", Ui.ACCENT);   // v35：块 → 脚本（恒可；保存时定锁）
        toScript.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = switchToScript();
                if (err != null) Ui.toast(err);
            }
        });
        TextView done = Ui.btnSmallPrimary(act, "完成");
        done.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = commitEdit();
                if (err != null) Ui.toast(err);
            }
        });
        bar.addView(toScript);
        bar.addView(cancel);
        bar.addView(done);

        container.removeAllViews();

        LinearLayout card = Ui.card(act);
        card.addView(Ui.tv(act, "操作 " + eo.name, 16, Ui.TEXT, true));
        card.addView(Ui.label(act, "门控区域（- = 无；仅开关型区域可选）"));
        List<String> gateOpts = new ArrayList<String>();
        gateOpts.add("-");
        gateOpts.addAll(switchIds);
        if (!"-".equals(eo.gate) && !switchIds.contains(eo.gate)) gateOpts.add(eo.gate);   // 旧值/已失效：保留展示，保存时校验
        final Forms.Chips[] gateRef = new Forms.Chips[1];
        gateRef[0] = new Forms.Chips(act, gateOpts, eo.gate, new Runnable() {
            public void run() { eo.gate = gateRef[0].selected(); }
        });
        card.addView(gateRef[0].view);
        if (switchIds.isEmpty()) {
            TextView hint = Ui.tv(act, "（还没有开关型区域——在区域编辑里把「开关型」打开）", 11, Ui.WARN, false);
            hint.setPadding(Ui.dp(act, 2), Ui.dp(act, 4), 0, 0);
            card.addView(hint);
        }
        Forms.SwitchRow rAuto = Forms.switchRow(act, "执行后自动关闭开关型门控",
                "开关型区域被本操作触发后自动复位", eo.autooff == 1, null);
        rAuto.sw.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            public void onCheckedChanged(android.widget.CompoundButton b, boolean v) {
                eo.autooff = v ? 1 : 0;
            }
        });
        card.addView(rAuto.row);
        container.addView(card);

        // 视图切换：卡片编辑 / 流程总览（紧凑时间线）
        final Forms.Seg[] viewSeg = new Forms.Seg[1];
        viewSeg[0] = new Forms.Seg(act, new String[]{"卡片", "流程总览"}, overview ? 1 : 0, new Runnable() {
            public void run() {
                overview = viewSeg[0].selected() == 1;
                clearSel(false);                    // v31：切视图退出连选
                render();
            }
        });
        LinearLayout segPad = Ui.col(act);
        segPad.setPadding(0, Ui.dp(act, 4), 0, Ui.dp(act, 6));
        segPad.addView(viewSeg[0].row);
        container.addView(segPad);

        if (eo.steps.isEmpty()) {                    // 空操作引导（新建草稿先落在这）——主操作在底栏，此处只指路
            TextView emptyHint = Ui.tv(act, "还没有步骤——点下方「＋ 添加步骤」开始", 13, Ui.DIM, false);
            emptyHint.setGravity(Gravity.CENTER);
            emptyHint.setPadding(0, Ui.dp(act, 22), 0, Ui.dp(act, 10));
            container.addView(emptyHint);
        }

        if (overview) {
            container.addView(StepFlow.build(act, eo.steps, new StepFlow.Tap() {
                public void onTap(int idx) { openStep(idx); }
            }, foldedBlocks, new StepFlow.Toggle() {   // v12.3：块头 ▼/▸ 展开收起
                public void onToggle(int headIdx) { toggleFoldAt(headIdx); }   // v31：与卡片视图共用收起集
            }, focusRows));                             // v26：行视图表（定位用）
        } else {
        // v12：块感知渲染（形态 C 色带块风）——块体嵌色带壳；块内行平铺、块外行卡片
        java.util.ArrayList<LinearLayout> bodyStack = new java.util.ArrayList<LinearLayout>();
        java.util.ArrayList<LinearLayout> parentStack = new java.util.ArrayList<LinearLayout>();
        LinearLayout listHost = container;
        testShellMargins.clear();                                          // v6B：测试直驱断言用（色带步进剖面）
        for (int i = 0; i < eo.steps.size(); i++) {
            final int idx = i;
            final Ops.Step s = eo.steps.get(i);
            if (s.type == Ops.T_LOOP || s.type == Ops.T_IF) {
                LinearLayout shell = Ui.col(act);
                shell.setBackground(Ui.blockBg(act,
                        s.type == Ops.T_LOOP ? Ui.ACCENT : Ui.COND_BAR,
                        s.type == Ops.T_LOOP ? Ui.LOOP_TINT : Ui.COND_TINT));
                LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
                // v6C 修（评审 F1）：margin 是相对父壳的，嵌套会累积 —— 每层只加 14dp（首层 0），
                // 绝对偏移才逐级 =14·D；padFor 的补偿在全深度精确成立（此前 14*size 在 D≥2 双重计数漂移）。
                slp.leftMargin = Ui.dp(act, bodyStack.size() == 0 ? 0 : 14);
                listHost.addView(shell, slp);
                testShellMargins.add(slp.leftMargin);
                int foldTail = -1;                                            // v31：卡片视图收起——块体整体跳过
                if (foldedBlocks.contains(s)) {
                    int tt = Ops.pairTail(eo.steps, i);
                    if (tt > i) foldTail = tt;                                // 配对失败按展开处理
                }
                LinearLayout headV = blockHeadRow(idx, s, bodyStack.size(),
                        foldTail > i ? (foldTail - idx - 1) : -1);            // v12.3 代码式缩进；v31 收起步数
                shell.addView(headV);
                focusRows.put(Integer.valueOf(idx), headV);                    // v26：行锚（整块移动/定位）
                bodyStack.add(shell);
                parentStack.add(listHost);
                listHost = shell;
                if (foldTail > i) i = foldTail - 1;                           // 收起：跳过块体（下轮 = 块尾行）
            } else if (s.type == Ops.T_ENDLOOP || s.type == Ops.T_ENDIF) {
                int bh = Ops.blockHeadOf(eo.steps, idx);              // v31：块尾挂在收起块的配对头 → 带计数 chip
                int tailFoldN = -1;
                if (bh >= 0 && foldedBlocks.contains(eo.steps.get(bh))) {
                    int tt = Ops.pairTail(eo.steps, bh);
                    if (tt == idx) tailFoldN = tt - bh - 1;
                }
                LinearLayout tailV = blockTailRow(idx, s, Math.max(0, bodyStack.size() - 1), tailFoldN);
                listHost.addView(tailV);
                focusRows.put(Integer.valueOf(idx), tailV);
                if (!bodyStack.isEmpty()) bodyStack.remove(bodyStack.size() - 1);
                if (!parentStack.isEmpty()) listHost = parentStack.remove(parentStack.size() - 1);
            } else if (s.type == Ops.T_ELSE) {
                LinearLayout elseV = elseRow(idx, s, Math.max(0, bodyStack.size() - 1));   // v12.2：分支分隔行（＋ = 否则分支末尾插入）
                listHost.addView(elseV);
                focusRows.put(Integer.valueOf(idx), elseV);
            } else if (s.type == Ops.T_CLAUSE) {
                LinearLayout clV = clauseRow(idx, s, Math.max(0, bodyStack.size() - 1)); // v6B：块头下编号子行（缩进=块头层）
                listHost.addView(clV);
                focusRows.put(Integer.valueOf(idx), clV);
            } else {
                LinearLayout stV = stepRow(idx, s, !bodyStack.isEmpty(), bodyStack.size());
                listHost.addView(stV);
                focusRows.put(Integer.valueOf(idx), stV);
            }
        }
        }

        // 底部常驻操作条：步数 + 添加（不随滚动，步数多时也随手可加）；v31：连选态 / 剪贴板 [粘贴（N 步）]
        bottomBar.removeAllViews();
        bottomBar.setVisibility(View.VISIBLE);
        bottomBar.addView(Ui.divider(act));
        LinearLayout foot = Ui.row(act);
        foot.setGravity(Gravity.CENTER_VERTICAL);
        foot.setPadding(Ui.dp(act, 14), Ui.dp(act, 8), Ui.dp(act, 12), Ui.dp(act, 8));
        if (selecting()) {
            final int[] sr = selRange();
            int scn = sr == null ? 0 : (sr[1] - sr[0] + 1);
            foot.addView(Ui.mono(act, "已选 " + scn + " 步", 12, Ui.ACCENT),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView cancelSel = Ui.btnFlat(act, "取消", Ui.DIM);
            cancelSel.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { clearSel(true); }
            });
            TextView copySel = Ui.btnSmallPrimary(act, "复制所选");
            copySel.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { copySelected(); }
            });
            foot.addView(cancelSel);
            foot.addView(copySel);
        } else {
            foot.addView(Ui.mono(act, "共 " + eo.steps.size() + " 步（上限 " + Ops.MAX_STEPS + "）", 12, Ui.DIM),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            if (!StepClip.empty()) {                    // v31：剪贴板非空 → 粘贴入口（粘完不清空，可多处粘）
                TextView paste = Ui.btnSmall(act, "粘贴（" + StepClip.size() + " 步）");
                paste.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { pasteDialog(); }
                });
                foot.addView(paste);
            }
            TextView addStep = Ui.btnSmallPrimary(act, "＋ 添加步骤");
            addStep.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if (editing.steps.size() >= Ops.MAX_STEPS) { Ui.toast("步骤数已达上限 " + Ops.MAX_STEPS); return; }
                    askTypeThenAdd(-1, "");
                }
            });
            foot.addView(addStep);
        }
        bottomBar.addView(foot);
    }

    /** v12.3：代码式缩进（14dp/级）：块头 / 否则 / 块尾 = 本层；块内步骤 = 下一层（口径同流程总览）。 */
    private int ind(int dep) { return Ui.dp(act, 12 + dep * 14); }

    /** v6B：行内边距补偿（嵌套壳 marginLeft 14dp/级后，文本基线仍 = ind(dep)）。 */
    private int padFor(int dep, int shellDepth) { return Ui.dp(act, 12 + (dep - shellDepth) * 14); }

    /** v6B：块壳左缘步进剖面（测试直驱断言用；render() 重填）。 */
    public final java.util.List<Integer> testShellMargins = new java.util.ArrayList<Integer>();

    /** v6B：编辑块头时带上其子条件链副本（LOOP/IF 恒非空；其余 = null）。 */
    private java.util.List<Ops.Step> clauseRowsFor(int idx, Ops.Step se) {
        if (idx < 0 || !(se.type == Ops.T_LOOP || se.type == Ops.T_IF)) return null;
        int tail = Ops.clauseTail(editing.steps, idx);
        java.util.ArrayList<Ops.Step> l = new java.util.ArrayList<Ops.Step>();
        for (int i = idx + 1; i <= tail; i++) l.add(editing.steps.get(i).copy());
        return l;
    }

    /** 子条件行（v6B）：编号 + 连接词摘要（暗色、缩进=块头层）；点击进入该子条件编辑。 */
    private LinearLayout clauseRow(final int idx, final Ops.Step s, final int dep) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(padFor(dep, dep), Ui.dp(act, 3), Ui.dp(act, 8), Ui.dp(act, 3));
        row.setBackground(Ui.pressable(act, 0x00000000, 0x2EFFFFFF, 8));
        row.setClickable(true);
        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { openStep(idx); }
        });
        row.addView(Ui.tv(act, (idx + 1) + ". " + s.clauseSummary(editing.varSlots()), 12.5f, Ui.DIM, false),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView mv = Ui.btnFlat(act, "↕ 移动", Ui.ACCENT);   // v26：子条件组内换序（求值序=短路序）
        mv.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { moveStepDialog(idx); }
        });
        row.addView(mv);
        TextView cpc = Ui.btnFlat(act, "复制", Ui.DIM);       // v31：单条子条件复制（粘贴落在可挂头的链上）
        cpc.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { copyOne(idx); }
        });
        row.addView(cpc);
        attachRowSel(row, idx);
        return row;
    }

    /** 进入步骤编辑页（idx ≥ 0 编辑已有步骤；-1 新增）。 */
    private void openStep(int idx) {
        stepEditing = (idx >= 0) ? editing.steps.get(idx).copy() : newStep(presetType);
        presetType = 0;   // 一次性（弹层选定后回默认）
        stepIndex = idx;
        stepReturnY = listSc.getScrollY();   // v27：记住进入前位置（返回时恢复，不回顶）
        clearSel(false);                     // v31：进编辑退出连选
        render();
    }

    private Ops.Step newStep(int type) {
        Ops.Step s = new Ops.Step();
        s.type = type;
        if (type == Ops.T_TAP) s.ms = 50;
        if (type == Ops.T_LOOP) { s.a1 = 0; s.a2 = 3; }   // 默认：循环 3 次
        if (type == Ops.T_TOAST) s.ms = 2000;             // v6D：提示默认显示 2 秒
        if (type == Ops.T_HL) s.ms = 3000;                // v6E：框区域默认显示 3 秒
        if (type == Ops.T_BOX) s.ms = 3000;               // v6F：框矩形默认显示 3 秒
        return s;
    }

    /** v12.1：先底部弹层选类型 → 再进步骤编辑页（插入位置 insertAt，-1 = 末尾；ctx = 落点语境，插入成功后提示）。 */
    private void askTypeThenAdd(final int insertAt, final String ctx) {
        clearSel(true);                      // v31：插入入口退出连选（取消弹层也不留残影）
        Ui.Dlg d = Forms.typeSheet(act, new Forms.TypePick() {
            public void onPick(int t) {
                presetType = t;
                insertBefore = insertAt;
                insertCtx = ctx;
                openStep(-1);
            }
        });
        Ui.showSheet(d);
    }

    /** v33：行「＋」——在所在行正下方插入新步骤（落点 = Ops.insertBelowAt 统一口径；结构破损报「先修复」）。 */
    private void addBelow(final int idx) {
        int at = Ops.insertBelowAt(editing.steps, idx);
        if (at < 0) { Ui.toast("结构未配对（先修复再插入）"); return; }
        if (editing.steps.size() >= Ops.MAX_STEPS) { Ui.toast("步骤数已达上限 " + Ops.MAX_STEPS); return; }
        askTypeThenAdd(at, Ops.insertBelowCtx(editing.steps, idx));
    }

    /** 步骤编辑页（全屏表单，替代原弹窗）。 */
    private void renderStepEditor() {
        bottomBar.setVisibility(View.GONE);
        final Ops.Step se = stepEditing;
        bar.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        statusLine.setText(stepIndex < 0 ? "添加步骤" : "编辑步骤 " + (stepIndex + 1));
        TextView cancelB = Ui.btnSmall(act, "取消");
        TextView saveB = Ui.btnSmallPrimary(act, "保存");
        bar.addView(cancelB);
        bar.addView(saveB);

        container.removeAllViews();
        stepEdit = null;
        final StepEdit edit = StepEdit.build(act, se, regionIds, switchIds,
                tmplNames, ptsNames, editing.varSlots(), visual, clauseRowsFor(stepIndex, se));
        stepEdit = edit;                     // v32：保存/测试直驱共用引用
        container.addView(edit.root);

        if (stepIndex >= 0) {
            TextView delB = Ui.btnSmallTonal(act, "删除此步", Ui.WARN);
            LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            dlp.setMargins(Ui.dp(act, 12), Ui.dp(act, 6), Ui.dp(act, 12), Ui.dp(act, 4));
            delB.setLayoutParams(dlp);
            delB.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    editing.steps.remove(stepIndex);
                    Ui.toast("已删除步骤 " + (stepIndex + 1));
                    stepEditing = null;
                    stepEdit = null;
                    clearSel(false);      // v31
                    render();
                    restoreScroll();   // v27：返回恢复原滚动位置
                }
            });
            container.addView(delB);
        }

        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { cancelStepEdit(); }
        });
        saveB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = saveStepEdit();
                if (err != null) Ui.toast(err);
            }
        });
    }

    /** v32：保存步骤编辑（[保存] 按钮与测试直驱共用）。返回错误文案或 null。
     *  编辑已有步 = 就地更新（块头 = 子条件组替换）；新增步 = 插入（块头同页配好的子条件随插一起落位）。 */
    public String saveStepEdit() {
        if (stepEditing == null || stepEdit == null) return "不在步骤编辑";
        Ops.Step se = stepEditing;
        String err = stepEdit.collect();
        if (err != null) return err;
        if (stepIndex >= 0) {
            java.util.List<Ops.Step> cl = stepEdit.collectedClauses();
            if (cl != null) {                  // v6B：块头（条件块 / 到条件循环）→ 头 + 子条件组替换
                if (!cl.isEmpty() && !Ops.canClause(se)) {
                    return "循环模式不是「到条件」：请先删除子条件（" + cl.size() + " 条）";
                }
                if (!Ops.setClauseGroup(editing.steps, stepIndex, se, cl)) {
                    return "无法保存子条件（超上限 " + Ops.CLAUSE_MAX + " 或步数超限）";
                }
                Ui.toast("步骤 " + (stepIndex + 1) + " 已更新"
                        + (cl.isEmpty() ? "" : "（子条件 " + cl.size() + " 条）"));
            } else {
                editing.steps.set(stepIndex, se);
                Ui.toast("步骤 " + (stepIndex + 1) + " 已更新");
            }
            insertBefore = -1;
            insertCtx = "";
            stepEditing = null;
            stepEdit = null;
            render();
            restoreScroll();   // v27：返回恢复原滚动位置
            return null;
        }
        /* v32 修：新增块同页配好的子条件随插入一起落位（原实现只看插入，clauses 被整体丢弃） */
        return insertNewStep(insertBefore, se, insertCtx, stepEdit.collectedClauses());
    }

    /** 取消步骤编辑（[取消] 按钮 / 测试直驱共用；v27：返回恢复滚动位置）。 */
    public void cancelStepEdit() {
        stepEditing = null;
        stepEdit = null;
        insertBefore = -1;
        insertCtx = "";
        render();
        restoreScroll();
    }

    /** v26：插入新步骤（含结构配套），收尾并定位到落点。at = 插入下标（<0 或越界 = 末尾）；
     *  ctx = 落点语境（成立分支/否则分支/循环体，可空）。返回错误文案或 null（供 TestBench 直驱）。 */
    public String insertNewStep(int at, Ops.Step se, String ctx) {
        return insertNewStep(at, se, ctx, null);
    }

    /** v32：clauses = 块头随附子条件（新增流程同页配置；null/空 = 无）。
     *  计数按「块头 + 结构配套 + 子条件」合计预检；失败不落位（保持编辑现场）。 */
    public String insertNewStep(int at, Ops.Step se, String ctx, java.util.List<Ops.Step> clauses) {
        if (editing == null) return "不在操作编辑中";
        int addC = (clauses == null) ? 0 : clauses.size();
        if (addC > 0) {
            if (!Ops.canClause(se)) return "循环模式不是「到条件」：请先删除子条件（" + addC + " 条）";
            if (1 + addC > Ops.CLAUSE_MAX) return "子条件超上限（含头子句共 " + Ops.CLAUSE_MAX + " 条）";
        }
        int atc = (at >= 0 && at <= editing.steps.size()) ? at : editing.steps.size();
        int extra = (se.type == Ops.T_LOOP) ? 1 : ((se.type == Ops.T_IF) ? 2 : 0);
        if (editing.steps.size() + 1 + extra + addC > Ops.MAX_STEPS) {
            return "步骤数将超过上限 " + Ops.MAX_STEPS + "（结构行/子条件按件计）";
        }
        if (!Ops.insertStep(editing.steps, atc, se)) {
            return "步骤数将超过上限 " + Ops.MAX_STEPS + "（结构行按配套计）";
        }
        if (addC > 0 && !Ops.setClauseGroup(editing.steps, atc, se, clauses)) {
            for (int k = 0; k < 1 + extra; k++) editing.steps.remove(atc);   // 回滚（理论不可达：计数已预检）
            return "无法保存子条件（超上限 " + Ops.CLAUSE_MAX + " 或步数超限）";
        }
        String suf = (ctx == null || ctx.isEmpty()) ? "" : " · " + ctx;
        String clSuf = (addC > 0) ? "（子条件 " + addC + " 条）" : "";
        if (se.type == Ops.T_LOOP) Ui.toast("已插入循环（含循环结束）" + clSuf + suf);
        else if (se.type == Ops.T_IF) Ui.toast("已插入条件块（含否则 / 结束）" + clSuf + suf);
        else Ui.toast("已插入步骤 " + (atc + 1) + suf);
        insertBefore = -1;
        insertCtx = "";
        stepEditing = null;
        stepEdit = null;
        clearSel(false);                     // v31
        render();
        focusStep(atc);
        return null;
    }

    // ---------------- v26：操作后定位（添加/移动 → 滚动到目标行） ----------------

    /** 请求定位到 idx 行：确保目标可见（块内已收起的先展开），下一帧执行最小滚动。 */
    private void focusStep(int idx) {
        if (idx < 0 || editing == null || idx >= editing.steps.size()) return;
        unfoldAncestorsOf(idx);                      // v31：收起块内的目标先展开（卡片/总览共用；内部会重渲染）
        focusIdxRequest = idx;
        try {
            container.post(new Runnable() {
                public void run() { applyFocusNow(); }
            });
        } catch (Throwable t) { /* 离屏环境无消息队列时静默 */ }
    }

    /** 展开覆盖 idx 的全部已收起块（总览模式下收起块不渲染行，定位会落空）。 */
    private void unfoldAncestorsOf(int idx) {
        if (foldedBlocks.isEmpty() || editing == null) return;
        boolean changed = false;
        for (int i = 0; i < editing.steps.size(); i++) {
            Ops.Step hs = editing.steps.get(i);
            if (!foldedBlocks.contains(hs)) continue;
            int tt = Ops.pairTail(editing.steps, i);
            if (i < idx && tt > idx) { foldedBlocks.remove(hs); changed = true; }
        }
        if (changed) render();
    }

    /** 立即执行定位（post 回调与测试直驱共用）：最小滚动使目标行连同边距进入视野；返回目标 Y（-1 = 无行/未布局）。
     *  ⚠ 时序：post 回调可能早于本帧布局（View.post 立即入队、布局等 vsync）——行在但未量好时**保留请求并重投**
     *  （≤10 次），量好即滚；行已不在（收起/删除）则放弃。 */
    public int applyFocusNow() {
        int idx = focusIdxRequest;
        if (idx < 0) return -1;
        View rv = focusRows.get(Integer.valueOf(idx));
        if (rv == null) { focusIdxRequest = -1; return -1; }        // 行不在 → 放弃
        int y = computeFocusScrollY(idx);
        if (y < 0 && focusTries < 10) {                             // 未量好 → 下一轮再试
            focusTries++;
            try {
                container.post(new Runnable() {
                    public void run() { applyFocusNow(); }
                });
            } catch (Throwable t) { focusIdxRequest = -1; }
            return -1;
        }
        focusTries = 0;
        focusIdxRequest = -1;
        if (y >= 0 && listSc != null) {
            try { listSc.smoothScrollTo(0, y); } catch (Throwable t) { /* 离屏静默 */ }
        }
        return y;
    }

    // ---------------- v27：步骤编辑返回 → 恢复进入前滚动位置 ----------------

    /** 请求恢复滚动位置（步骤编辑返回时用）；未量好由 applyRestoreNow 重投（≤10 次）。 */
    private void restoreScroll() {
        if (stepReturnY > 0) {
            restoreYRequest = stepReturnY;
            try {
                container.post(new Runnable() {
                    public void run() { applyRestoreNow(); }
                });
            } catch (Throwable t) { /* 离屏无消息队列：静默（由测试直驱） */ }
        }
        stepReturnY = -1;
    }

    /** 立即恢复滚动（post 与测试直驱共用）；返回实际 Y（-1 = 未量好已重投 / 无请求）。 */
    public int applyRestoreNow() {
        int y = restoreYRequest;
        if (y < 0) return -1;
        int viewportH = listSc.getHeight() - listSc.getPaddingTop() - listSc.getPaddingBottom();
        if (viewportH <= 0 || container.getHeight() <= 0) {          // 未布局 → 下一轮再试
            if (restoreTries++ < 10) {
                try {
                    container.post(new Runnable() {
                        public void run() { applyRestoreNow(); }
                    });
                } catch (Throwable t) { restoreTries = 0; }
            } else restoreTries = 0;
            return -1;
        }
        restoreTries = 0;
        int maxScroll = Math.max(0, container.getHeight() - viewportH);
        int target = Math.min(y, maxScroll);
        try { listSc.scrollTo(0, target); } catch (Throwable t) { /* 离屏静默 */ }
        restoreYRequest = -1;
        return target;
    }

    /** v27：测试直驱——进入步骤编辑前的捕获位置。 */
    public int stepReturnYForTest() { return stepReturnY; }

    /** v27：测试直驱——待恢复请求。 */
    public int restoreYRequestForTest() { return restoreYRequest; }

    /** v27：测试直驱——直接滚动列表。 */
    public void listScrollToForTest(int y) { listSc.scrollTo(0, y); }

    /** v27：测试直驱——当前滚动位置。 */
    public int listScrollYForTest() { return listSc.getScrollY(); }

    /** v27：测试直驱——等价点步骤行（进入步骤编辑）。 */
    public void openStepForTest(int idx) { openStep(idx); }

    /** v32：测试直驱——进入「新增步骤」编辑页（等价添加步骤弹层选定类型；at = 插入位，-1 = 末尾）。 */
    public void startNewStepForTest(int type, int at, String ctx) {
        presetType = type;
        insertBefore = at;
        insertCtx = ctx == null ? "" : ctx;
        openStep(-1);
    }

    /** v32：测试直驱——当前步骤编辑页构建器（null = 不在步骤编辑）。 */
    public StepEdit stepEditorForTest() { return stepEdit; }

    /** 行顶/行底（相对 content）/ 视口高 / 目标 Y 观测（测试直驱）；无行时各 -1。 */
    public int[] focusMetricsForTest(int idx) {
        int[] r = new int[]{-1, -1, -1, -1};
        View rv = focusRows.get(Integer.valueOf(idx));
        if (rv == null || listSc == null) return r;
        int top = 0;
        View v = rv;
        while (v != null && v != container) { top += v.getTop(); v = (View) v.getParent(); }
        if (v == null) return r;
        r[0] = top;
        r[1] = top + rv.getHeight();
        r[2] = listSc.getHeight() - listSc.getPaddingTop() - listSc.getPaddingBottom();
        r[3] = computeFocusScrollY(idx);
        return r;
    }

    private int computeFocusScrollY(int idx) {
        View rv = focusRows.get(Integer.valueOf(idx));
        if (rv == null || listSc == null || container == null) return -1;
        int top = 0;
        View v = rv;
        while (v != null && v != container) { top += v.getTop(); v = (View) v.getParent(); }
        if (v == null) return -1;
        int rowH = rv.getHeight();
        int viewportH = listSc.getHeight() - listSc.getPaddingTop() - listSc.getPaddingBottom();
        if (rowH <= 0 || viewportH <= 0) return -1;         // 未布局（离屏需先 measure/layout）
        int margin = Ui.dp(act, 12);
        int cur = listSc.getScrollY();
        int target = cur;
        if (top - margin < cur) target = top - margin;                              // 目标在上方
        else if (top + rowH + margin > cur + viewportH)                             // 目标在下方
            target = top + rowH + margin - viewportH;
        int maxScroll = Math.max(0, container.getHeight() - viewportH);
        if (target < 0) target = 0;
        if (target > maxScroll) target = maxScroll;
        return target;
    }

    /** 一行步骤（v12）：块外 = 卡片；块内 = 平铺行（色带壳内）。dep = 代码式缩进层级（v12.3）。 */
    private LinearLayout stepRow(final int idx, final Ops.Step s, final boolean inBlock, final int dep) {
        LinearLayout sc = inBlock ? Ui.col(act) : Ui.card(act);
        if (!inBlock && s.known()) {
            sc.setBackground(Ui.pressableStroked(act, Ui.CARD, Ui.CARD_LINE, 0xFF23272C, 8));
        }
        if (inBlock) {
            sc.setPadding(padFor(dep, dep - 1), Ui.dp(act, 6), Ui.dp(act, 10), Ui.dp(act, 6));
            sc.setBackground(Ui.pressable(act, 0x00000000, 0x2EFFFFFF, 8));
        }
        if (s.known()) {
            sc.setClickable(true);
            sc.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { openStep(idx); }
            });
        }
        LinearLayout stRow = Ui.row(act);
        stRow.addView(Ui.tv(act, (idx + 1) + ". " + Ops.typeName(s.type), inBlock ? 13.5f : 14, Ui.TEXT, true));
        if (!s.known()) stRow.addView(Ui.chip(act, "未解锁", Ui.WARN));
        sc.addView(stRow);
        TextView sum = Ui.tv(act, s.summary(editing.varSlots()), inBlock ? 12.5f : 13, Ui.DIM, false);
        sum.setPadding(0, Ui.dp(act, 3), 0, 0);
        sc.addView(sum);
        LinearLayout btns = Ui.row(act);
        btns.setGravity(Gravity.END);
        if (s.known()) {
            TextView add = Ui.btnFlat(act, "＋", Ui.ACCENT);   // v33：在本行下方插入（原 [✎ 编辑] 退役——整卡点击即编辑，去重复功能）
            add.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { addBelow(idx); }
            });
            btns.addView(add);
        } else {
            btns.addView(Ui.tv(act, "类型未解锁（可移动/复制/删除）", 11, Ui.WARN, false));
        }
        TextView mv = Ui.btnFlat(act, "↕ 移动", Ui.ACCENT);
        mv.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { moveStepDialog(idx); }
        });
        TextView cpy = Ui.btnFlat(act, "复制", Ui.DIM);       // v31：单步复制（块头=整块，见 blockHeadRow）
        cpy.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { copyOne(idx); }
        });
        TextView del = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
        del.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                editing.steps.remove(idx);
                clearSel(false);                     // v31
                render();
                Ui.toast("已删除步骤 " + (idx + 1));
            }
        });
        btns.addView(mv);
        btns.addView(cpy);
        btns.addView(del);
        sc.addView(btns);
        attachRowSel(sc, idx);                       // v31：长按进入连选
        return sc;
    }

    /** 块头行（v12/v12.2）：类型 + 参数摘要（琥珀粗体）；点行=编辑块属性。dep = 代码式缩进层级（v12.3）。
     *  v33 起 [＋] = 紧跟块头下方插入（子条件之后、块体开头）；「加到块体末尾」改用块内末行 / 块尾行的 [＋]。
     *  v31：▼/▸ = 卡片视图收起开关（与总览共用收起集；收起计数 chip 落在块尾行——块头行 4 个控件后已无宽度）。 */
    private LinearLayout blockHeadRow(final int idx, final Ops.Step s, final int dep, final int foldN) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(padFor(dep, dep), Ui.dp(act, 7), Ui.dp(act, 8), Ui.dp(act, 7));
        row.setBackground(Ui.pressable(act, 0x00000000, 0x2EFFFFFF, 8));
        row.setClickable(true);
        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { openStep(idx); }
        });
        LinearLayout col = Ui.col(act);
        col.addView(Ui.tv(act, (idx + 1) + ". " + Ops.typeName(s.type), 14, Ui.ACCENT, true));
        TextView sum = Ui.tv(act, s.summary(editing.varSlots()), 12.5f, Ui.DIM, false);
        sum.setPadding(0, Ui.dp(act, 2), 0, 0);
        col.addView(sum);
        row.addView(col, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView mv = Ui.btnFlat(act, "↕ 移动", Ui.ACCENT);   // v26：整块移动（子条件/块体/否则/块尾一起走）
        mv.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { moveStepDialog(idx); }
        });
        row.addView(mv);
        TextView cpy = Ui.btnFlat(act, "复制", Ui.DIM);       // v31：整块复制（含子条件/块体/否则/块尾）
        cpy.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { copyOne(idx); }
        });
        row.addView(cpy);
        TextView add = Ui.btnFlat(act, "＋", Ui.ACCENT);   // v33：紧跟块头下方插入（子条件之后、块体开头）
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { addBelow(idx); }
        });
        row.addView(add);
        final TextView chev = Ui.tv(act, foldN >= 0 ? "▸" : "▼", 13, Ui.DIM, false);   // v31：收起开关（同总览）
        chev.setGravity(Gravity.CENTER);
        chev.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(act, 40), Ui.dp(act, 40)));
        chev.setBackground(Ui.pressable(act, 0x00000000, 0x30FFFFFF, 8));
        chev.setClickable(true);
        chev.setFocusable(true);
        chev.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { toggleFoldAt(idx); }
        });
        row.addView(chev);
        attachRowSel(row, idx);
        return row;
    }

    /** 块尾行（v12）：灰小字（循环结束 / 条件块结束）；v31 收起时在名字右侧挂「已收起 N 步」chip；
     *  v33：[＋] = 整块之后插入；[＋ 否则]（条件块无否则时）+ [✕ 整块]（两步确认）。 */
    private LinearLayout blockTailRow(final int idx, final Ops.Step s, final int dep, final int foldN) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(padFor(dep, dep), Ui.dp(act, 3), Ui.dp(act, 8), Ui.dp(act, 5));
        row.addView(Ui.tv(act, Ops.typeName(s.type), 11.5f, Ui.FAINT, false));
        if (foldN >= 0) row.addView(Ui.chip(act, "已收起 " + foldN + " 步", Ui.DIM));   // v31：收起计数
        View sp = new View(act);
        row.addView(sp, new LinearLayout.LayoutParams(0, 1, 1f));
        TextView add = Ui.btnFlat(act, "＋", Ui.ACCENT);   // v33：在整块之后插入
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { addBelow(idx); }
        });
        row.addView(add);
        if (s.type == Ops.T_ENDIF && !hasElse(idx)) {
            TextView addElse = Ui.btnFlat(act, "＋ 否则", Ui.DIM);
            addElse.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    Ops.Step nl = new Ops.Step();
                    nl.type = Ops.T_ELSE;
                    editing.steps.add(idx, nl);          // 插到条件块结束之前
                    clearSel(false);                     // v31
                    render();
                    Ui.toast("已加「否则」");
                }
            });
            row.addView(addElse);
        }
        final TextView del = Ui.btnFlat(act, "✕ 整块", Ui.FAINT);
        del.setOnClickListener(new View.OnClickListener() {
            private boolean armed = false;
            public void onClick(View v) {
                if (!armed) { armed = true; del.setText("确认删块"); del.setTextColor(Ui.WARN); return; }
                removeBlock(idx);
            }
        });
        row.addView(del);
        attachRowSel(row, idx);
        return row;
    }

    /** 「否则」分隔行（v12.2）：结构行（不入步骤编辑）+ [＋]（v33：否则行下方 = 否则分支开头插入）+ [✕ 否则]（两步确认，删标记本身）。
     *  删除只移除结构行「否则」；其下步骤随之归入成立分支一侧（沿原有单行删除语义，未改）。 */
    private LinearLayout elseRow(final int idx, final Ops.Step s, final int dep) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(padFor(dep, dep), Ui.dp(act, 5), Ui.dp(act, 8), Ui.dp(act, 5));
        row.addView(Ui.tv(act, (idx + 1) + ". 否则", 12.5f, Ui.DIM, true));   // 带步号：与流程总览一致，防「跳号」误读
        View line = new View(act);
        line.setBackgroundColor((Ui.COND_BAR & 0x00FFFFFF) | 0x59000000);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(0, Ui.dp(act, 1), 1f);
        llp.leftMargin = Ui.dp(act, 10);
        llp.rightMargin = Ui.dp(act, 6);
        line.setLayoutParams(llp);
        row.addView(line);
        TextView add = Ui.btnFlat(act, "＋", Ui.ACCENT);   // v33：否则行下方插入（否则分支开头）
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { addBelow(idx); }
        });
        row.addView(add);
        final TextView del = Ui.btnFlat(act, "✕ 否则", Ui.FAINT);
        del.setOnClickListener(new View.OnClickListener() {
            private boolean armed = false;
            public void onClick(View v) {
                if (!armed) { armed = true; del.setText("确认删除"); del.setTextColor(Ui.WARN); return; }
                editing.steps.remove(idx);
                clearSel(false);                     // v31
                render();
                Ui.toast("已删除「否则」（其下步骤并入成立分支）");
            }
        });
        row.addView(del);
        attachRowSel(row, idx);
        return row;
    }

    /** 条件块（endif 下标）本层是否已有「否则」。 */
    private boolean hasElse(int endifIdx) {
        int d = 0;
        for (int i = endifIdx - 1; i >= 0; i--) {
            int t = editing.steps.get(i).type;
            if (t == Ops.T_ENDIF || t == Ops.T_ENDLOOP) d++;
            else if (t == Ops.T_LOOP || t == Ops.T_IF) {
                if (d == 0) return false;                // 到本层头了，没见 15
                d--;
            } else if (t == Ops.T_ELSE && d == 0) {
                return true;                             // 本层 15
            }
        }
        return false;
    }

    /** 删整块：从头（配对头）删到配对尾（栈法，含内嵌步）。 */
    private void removeBlock(int tailIdx) {
        int d = 0, head = -1;
        for (int i = tailIdx; i >= 0; i--) {
            int t = editing.steps.get(i).type;
            if (t == Ops.T_ENDIF || t == Ops.T_ENDLOOP) d++;
            else if (t == Ops.T_LOOP || t == Ops.T_IF) {
                d--;
                if (d == 0) { head = i; break; }
            }
        }
        if (head < 0) { Ui.toast("结构未配对（先修复）"); return; }
        int depth = 0, end = head;
        for (int i = head; i < editing.steps.size(); i++) {
            int t = editing.steps.get(i).type;
            if (t == Ops.T_LOOP || t == Ops.T_IF) depth++;
            else if (t == Ops.T_ENDLOOP || t == Ops.T_ENDIF) {
                depth--;
                if (depth == 0) { end = i + 1; break; }
            }
        }
        int n = end - head;
        for (int k = 0; k < n; k++) editing.steps.remove(head);
        clearSel(false);                     // v31
        render();
        Ui.toast("已删除整块（" + n + " 步）");
    }

    /** v26：「移动」对话框——普通步=自身、块头=整块（含子条件/块体/否则/块尾）一起移动，一步到位。
     *  子条件行走组内换序对话框。k = 单元首行最终落位（1 基）；只列合法落点（越界/原位不列）。 */
    private void moveStepDialog(final int idx) {
        if (editing == null || idx < 0 || idx >= editing.steps.size()) return;
        final Ops.Step s = editing.steps.get(idx);
        if (s.type == Ops.T_CLAUSE) { clauseMoveDialog(idx); return; }   // v26：19 行只组内换序
        final int n = editing.steps.size();
        if (n <= 1) { Ui.toast("只有一步，无需移动"); return; }
        final int tail = Ops.unitTail(editing.steps, idx);
        if (tail < 0) { Ui.toast("结构未配对（先修复再移动）"); return; }
        final int m = tail - idx + 1;
        final int maxK = n - m + 1;               // 单元首行的最大合法落位
        final List<String> items = new ArrayList<String>();
        final List<Integer> ks = new ArrayList<Integer>();
        for (int k = 1; k <= maxK; k++) {
            if (k == idx + 1) continue;           // 跳过原位
            String label;
            if (k == 1) label = "移到第 1 位（开头）";
            else if (k == maxK) label = (m > 1) ? ("移到末尾（第 " + maxK + " 位）")
                                                : ("移到第 " + maxK + " 位（末尾）");
            else label = "移到第 " + k + " 位";
            items.add(label);
            ks.add(Integer.valueOf(k));
        }
        if (items.isEmpty()) { Ui.toast("没有可移动的位置"); return; }
        String title = "移动「" + (idx + 1) + ". " + Ops.typeName(s.type) + "」"
                + (m > 1 ? "整块（" + m + " 步）" : "") + "到…";
        Forms.pickDialog(act, title, items, -1, new Forms.Pick() {
            public void onPick(int index) {
                int k = ks.get(index).intValue();
                String err = moveUnitTo(idx, k);
                if (err != null) { Ui.toast(err); return; }
                Ui.toast(m > 1 ? ("已移到第 " + k + " 位（整块 " + m + " 步）")
                               : ("已移到第 " + k + " 位"));
            }
        });
    }

    /** v26：子条件行移动——只在所属块头的子条件组内换序（求值序 = 短路序）。 */
    private void clauseMoveDialog(final int idx) {
        if (editing == null || idx < 0 || idx >= editing.steps.size()) return;
        final int head = Ops.blockHeadOf(editing.steps, idx);
        if (head < 0) { Ui.toast("结构未配对（先修复再移动）"); return; }
        final int rows = Ops.clauseCount(editing.steps, head) - 1;   // 19 行条数（不含头子句）
        final int cur = idx - head;                                  // 当前条位（1 基）
        if (rows <= 1) { Ui.toast("组内只有 1 条子条件，无需移动"); return; }
        final List<String> items = new ArrayList<String>();
        final List<Integer> js = new ArrayList<Integer>();
        for (int j = 1; j <= rows; j++) {
            if (j == cur) continue;
            items.add("移到第 " + j + " 条（共 " + rows + " 条）");
            js.add(Integer.valueOf(j));
        }
        Forms.pickDialog(act, "移动「" + (idx + 1) + ". " + editing.steps.get(idx).clauseSummary(editing.varSlots())
                + "」到…（组内）", items, -1, new Forms.Pick() {
            public void onPick(int index) {
                int j = js.get(index).intValue();
                String err = moveClauseTo(idx, j);
                if (err != null) { Ui.toast(err); return; }
                Ui.toast("已移到组内第 " + j + " 条");
            }
        });
    }

    /** v26：移动单元（普通步=自身；块头=整块）到第 k 位（k = 单元首行最终落位，1 基）。
     *  落点全程结构预检（结构扫描 + 跨块跳）：会把原本完好结构弄坏 → 回滚并返回原因；原本就破损的数据不拦。
     *  成功 = 渲染 + 定位到新位置。返回错误文案或 null（供 TestBench 直驱）。 */
    public String moveUnitTo(int idx, int k) {
        if (editing == null || idx < 0 || idx >= editing.steps.size()) return "不在操作编辑中";
        int tail = Ops.unitTail(editing.steps, idx);
        if (tail < 0) return "结构未配对（先修复再移动）";
        int m = tail - idx + 1;
        int n = editing.steps.size();
        if (k < 1 || k > n - m + 1) return "目标位非法";
        if (k == idx + 1) return "目标与原位相同";
        List<Ops.Step> pre = new ArrayList<Ops.Step>(editing.steps);
        String preErr = Ops.structureErr(pre);
        java.util.List<Ops.Step> mv = new ArrayList<Ops.Step>();
        for (int i = idx; i <= tail; i++) mv.add(editing.steps.get(i));
        editing.steps.subList(idx, tail + 1).clear();
        editing.steps.addAll(k - 1, mv);
        String postErr = Ops.structureErr(editing.steps);
        if (postErr != null && preErr == null) {
            editing.steps.clear();
            editing.steps.addAll(pre);
            return "落点不合法：" + postErr;
        }
        clearSel(false);                     // v31
        render();
        focusStep(k - 1);
        return null;
    }

    /** v26：子条件行组内移动（j = 目标条位，1 基，仅计 19 行）。返回错误文案或 null（供 TestBench 直驱）。 */
    public String moveClauseTo(int idx, int j) {
        if (editing == null || idx < 0 || idx >= editing.steps.size()) return "不在操作编辑中";
        if (editing.steps.get(idx).type != Ops.T_CLAUSE) return "该步骤不是子条件行";
        int head = Ops.blockHeadOf(editing.steps, idx);
        if (head < 0) return "结构未配对（先修复再移动）";
        int rows = Ops.clauseCount(editing.steps, head) - 1;
        int cur = idx - head;
        if (j < 1 || j > rows) return "目标条位非法";
        if (j == cur) return "目标与原位相同";
        Ops.Step mv = editing.steps.remove(idx);
        editing.steps.add(head + j, mv);
        String err = Ops.structureErr(editing.steps);   // 防御：组内换序不应破结构
        if (err != null) {
            editing.steps.remove(head + j);
            editing.steps.add(idx, mv);
            return "落点不合法：" + err;
        }
        clearSel(false);                     // v31
        render();
        focusStep(head + j);
        return null;
    }

    /** [完成]/[保存]：校验并提交（草稿 editIndex<0 = 创建、落回列表；已有的 = 更新）。返回错误文案或 null（供 TestBench 直驱）。
     *  v35（T6）：两种视图都归一到 v8 落盘——脚本视图存原文（侧线定锁）；块视图按步骤生成规范脚本后编译落盘。 */
    public String commitEdit() {
        final boolean isNew = editIndex < 0;
        if (editing == null) return "不在编辑中";
        final boolean scriptMode = "脚本".equals(editMode) && scriptEdit != null;
        String err;
        final String srcFinal;
        final byte[] bytes;
        if (scriptMode) {
            ScriptStore.Save s = ScriptStore.prepare(scriptEdit.text(), "脚本");
            if (s.err != null) {
                scriptEdit.showError(s.line, s.err);
                return "脚本" + (s.line > 0 ? " 第 " + s.line + " 行" : "") + "：" + s.err;
            }
            srcFinal = s.src;
            bytes = s.bytes;
        } else {
            Ops.Op chk = editing.copy();
            chk.progHash = null;
            chk.progSize = 0;
            err = Ops.validate(chk, file, isNew ? null : file.ops.get(editIndex));
            if (err != null) return err;
            err = regionRefErr(editing);
            if (err != null) return err;
            if (chk.steps.isEmpty()) return "操作没有任何步骤（至少 1 步）";
            VtFromSteps.R tr = VtFromSteps.translate(chk);
            if (tr.src == null) return "脚本生成失败：" + (tr.problems.isEmpty() ? "（未知）" : tr.problems.get(0));
            ScriptStore.Save s = ScriptStore.prepare(tr.src, "块");
            if (s.err != null) return "编译失败：" + s.err + (s.line > 0 ? "（第 " + s.line + " 行）" : "");
            srcFinal = s.src;
            bytes = s.bytes;
        }
        // 门控校验（两种视图共同；核心对非开关门控 = 整条丢弃）
        String gate = editing.gate == null || editing.gate.isEmpty() ? "-" : editing.gate;
        if (!"-".equals(gate) && !switchIds.contains(gate)) {
            return "操作「" + editing.name + "」的门控「" + gate + "」不是开关型区域（核心会拦截此操作）";
        }
        // 开关判断引用提醒（脚本视图；不拦——运行时中止在核心侧）
        String switchWarn = null;
        if (scriptMode) {
            java.util.regex.Matcher m =
                    java.util.regex.Pattern.compile("开关开\\(\"([^\"]+)\"\\)").matcher(srcFinal);
            while (m.find()) {
                if (!switchIds.contains(m.group(1))) { switchWarn = m.group(1); break; }
            }
        }
        // v8 记录：元数据 + 产物（steps 恒空——程序在 progs/）
        Ops.Op recOp = new Ops.Op();
        recOp.name = editing.name;
        recOp.gate = gate;
        recOp.autooff = editing.autooff;
        recOp.extra = editing.extra;
        recOp.progHash = OpsV8.fnv8(bytes);
        recOp.progSize = bytes.length;
        if (isNew) file.ops.add(recOp);
        else file.ops.set(editIndex, recOp);
        dirty = true;
        exitScriptSession();
        editing = null;
        editIndex = -1;
        render();
        String what = (isNew ? "操作「" + recOp.name + "」已创建" : "操作「" + recOp.name + "」已更新")
                + (switchWarn != null ? "（注意：开关判断引用「" + switchWarn + "」不是开关型区域）" : "");
        saveV8(recOp, srcFinal, bytes, what);
        return null;
    }

    // ---------------- v35（T6 双视图）：脚本会话 / 转换 / v8 落盘 ----------------

    /** v36：脚本 [格式化]——规范重排（VtPrint 直通注释；侧线不变）。已是规范形态只提示。返回错误或 null。 */
    public String formatScript() {
        if (editing == null || scriptEdit == null) return "不在脚本编辑中";
        String t = scriptEdit.text();
        ScriptStore.Fmt f = ScriptStore.format(t);
        if (f.err != null) {
            scriptEdit.showError(f.line, f.err);
            return "无法格式化" + (f.line > 0 ? "（第 " + f.line + " 行）" : "") + "：" + f.err;
        }
        if (f.src.equals(t)) { Ui.toast("已是规范形态"); return null; }
        scriptEdit.setTextPreservingView(f.src);
        Ui.toast("已格式化");
        return null;
    }

    /** 脚本会话收场（状态复位；视图切换由 render() 完成）。 */
    private void exitScriptSession() {
        editMode = "块";
        scriptEdit = null;
        scriptSeed = null;
    }

    /** 块 → 脚本（恒可：按当前步骤生成规范脚本）。返回错误文案或 null（供测试直驱）。 */
    public String switchToScript() {
        if (editing == null) return "不在编辑中";
        Ops.Op chk = editing.copy();
        chk.progHash = null;
        chk.progSize = 0;
        String gen = "";
        if (!chk.steps.isEmpty()) {
            VtFromSteps.R tr = VtFromSteps.translate(chk);
            if (tr.src == null) return "脚本生成失败：" + (tr.problems.isEmpty() ? "（未知）" : tr.problems.get(0));
            gen = tr.src;
        }
        scriptSeed = gen;
        scriptEdit = null;                   // 重建（新会话）
        editMode = "脚本";
        render();
        return null;
    }

    /** 脚本 → 块（可视化子集内：解析+投影；否则返回原因文案）。 */
    public String switchToBlock() {
        if (editing == null || scriptEdit == null) return "不在脚本编辑中";
        String text = scriptEdit.text();
        VtToSteps.R p = VtToSteps.project(text);
        if (p.err != null) return p.err;
        editing.steps.clear();
        editing.steps.addAll(p.op.steps);
        if (!p.exact) Ui.toast("文本已按规范重排（空格 / 缺省参数展开）");
        editMode = "块";
        scriptEdit = null;
        render();
        return null;
    }

    /** 脚本编辑页（双视图·脚本侧）：bar=[状态][转块][取消][保存]；内容 = ScriptEdit。 */
    private void renderScriptEditor() {
        final Ops.Op eo = editing;
        if (listSc != null) listSc.setVisibility(View.GONE);
        bottomBar.setVisibility(View.GONE);
        bar.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        statusLine.setText("脚本编辑：" + eo.name);
        TextView fmt = Ui.btnFlat(act, "格式化", Ui.ACCENT);   // v36：规范重排（注释保留；侧线不变）
        fmt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = formatScript();
                if (err != null) Ui.toast(err);
            }
        });
        TextView toBlock = Ui.btnFlat(act, "转块", Ui.ACCENT);
        toBlock.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = switchToBlock();
                if (err != null) Ui.toast(err);
            }
        });
        TextView cancel = Ui.btnSmall(act, "取消");
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                boolean draft = editIndex < 0;
                exitScriptSession();
                editing = null;
                editIndex = -1;
                render();
                if (draft) Ui.toast("已取消：未创建操作");
            }
        });
        TextView saveB = Ui.btnSmallPrimary(act, "保存");
        saveB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = commitEdit();
                if (err != null) Ui.toast(err);
            }
        });
        bar.addView(fmt);
        bar.addView(toBlock);
        bar.addView(cancel);
        bar.addView(saveB);

        if (scriptEdit == null) {
            scriptEdit = ScriptEdit.build(act, scriptSeed == null ? "" : scriptSeed, metaSummary(eo),
                    new View.OnClickListener() {
                        public void onClick(View v) { opPropsDialog(); }
                    });
            scriptEdit.enablePickKeys(new Runnable() {          // v39：两枚取点键
                public void run() { pickScriptFrame(); }
            }, new Runnable() {
                public void run() { pickScriptScreen(); }
            });
        }
        editorHost.removeAllViews();
        editorHost.addView(scriptEdit.root, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));
        editorHost.setVisibility(View.VISIBLE);
    }

    /** 脚本模式：门控/自动关属性对话框（与块编辑页同源组件）。 */
    private void opPropsDialog() {
        final Ops.Op eo = editing;
        if (eo == null) return;
        List<String> gateOpts = new ArrayList<String>();
        gateOpts.add("-");
        gateOpts.addAll(switchIds);
        if (!"-".equals(eo.gate) && !switchIds.contains(eo.gate)) gateOpts.add(eo.gate);   // 旧值/失效值保留展示
        final Ui.Dlg d = Ui.dialog(act, "操作属性 · " + eo.name);
        d.body.addView(Ui.label(act, "门控区域（- = 无；仅开关型区域可选）"));
        final Forms.Chips[] gateRef = new Forms.Chips[1];
        gateRef[0] = new Forms.Chips(act, gateOpts, eo.gate, new Runnable() {
            public void run() { eo.gate = gateRef[0].selected(); }
        });
        d.body.addView(gateRef[0].view);
        Forms.SwitchRow rAuto = Forms.switchRow(act, "执行后自动关闭开关型门控",
                "开关型区域被本操作触发后自动复位", eo.autooff == 1, null);
        rAuto.sw.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            public void onCheckedChanged(android.widget.CompoundButton b, boolean v) {
                eo.autooff = v ? 1 : 0;
            }
        });
        d.body.addView(rAuto.row);
        TextView ok = Ui.btnSmallPrimary(act, "完成");
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                d.dismiss();
                render();
            }
        });
        d.actions.addView(ok);
        d.show();
    }

    /** 脚本编辑页头部摘要：门控 / 自动关。 */
    private String metaSummary(Ops.Op eo) {
        return "门控 " + ("-".equals(eo.gate) || eo.gate == null || eo.gate.isEmpty() ? "无" : eo.gate)
                + " · 自动关 " + (eo.autooff == 1 ? "开" : "关");
    }

    // ================= v39：脚本取点 =================

    /** [帧上取点]：帧会话取点（与块编辑 [取点 x,y] 同款）→ [确认] 插入脚本光标处。 */
    private void pickScriptFrame() {
        if (scriptEdit == null || editing == null) return;
        VisualPage.Session s = new VisualPage.Session();
        s.mode = VisualPage.MODE_PICK_POINT;
        VisualPage.open(s, new VisualPage.Done() {
            public void onTmplSaved(String name) { }
            public void onPointPicked(int lx, int ly) { insertScriptPick(lx, ly); }
            public void onColorPicked(int rgb) { }
            public void onCanceled() { }
        });
    }

    /** [屏幕取点]：武装悬浮窗遮罩（点真实屏幕，不误触游戏）→ 抓帧回跳 → 插入光标处。 */
    private void pickScriptScreen() {
        if (scriptEdit == null || editing == null) return;
        CaptureChip.startPick(act, "script");
        try {
            if (act instanceof android.app.Activity) ((android.app.Activity) act).moveTaskToBack(true);
        } catch (Throwable ig) { }
    }

    /** v39：把取到的竖屏逻辑坐标插入脚本光标处。返回=false：没有活脚本会话（屏幕取点回跳/MainActivity 路由入口）。 */
    public boolean insertScriptPick(int lx, int ly) {
        if (scriptEdit == null || editing == null || !"脚本".equals(editMode)) return false;
        scriptEdit.insertPickedPoint(lx, ly);
        return true;
    }

    /** v35（T6）：v8 操作落盘——.vts + .vtbc + ops.conf（元数据）+ 备份 + 镜像；完成后重渲染 + 生效口径。 */
    private void saveV8(final Ops.Op recOp, final String src, final byte[] bytes, final String what) {
        final String ov = pathOverride;
        final String path = ov != null ? ov : Ops.PATH;
        final String confText = file.serialize();
        Ui.bg(new Runnable() {
            public void run() {
                String err = null;
                if (ov == null) {
                    Su.backup(Ops.PATH);
                    Su.backup(OpsV8.srcPath(OpsV8.base(), recOp.name));
                    Su.backup(OpsV8.progPath(OpsV8.base(), recOp.name));
                }
                Su.R w1 = OpsV8.writeSrc(OpsV8.base(), recOp.name, src);
                Su.R w2 = w1.ok() ? OpsV8.writeProg(OpsV8.base(), recOp.name, bytes) : w1;
                Su.R w3 = w2.ok() ? Su.writeAtomic(path, confText.getBytes()) : w2;
                if (!w3.ok()) err = w3.trimmed();
                if (err == null && ov == null) Schemes.mirror();
                final String ferr = err;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (ferr != null) { Ui.toast("保存失败：" + ferr); render(); return; }
                        dirty = false;
                        render();
                        Ui.saved(what);
                    }
                });
            }
        }, null);
    }

    // （旧版「步骤弹窗 + StepForm」已重构为全屏步骤编辑页：见 renderStepEditor / StepEdit.java；
    //   旧版 ↑/↓ 逐步交换已重构为「↕ 移动」到第 N 位对话框：见 moveStepDialog）

    /** 测试台直驱（离屏渲染）：把编辑态直接置为该操作并渲染（不读盘、不起异步；供 TestBench 用）。
     *  注意：render() 有 file == null 守卫，直驱路径必须先补一个空 Ops.File。 */
    public void openEditorForTest(Ops.Op op) {
        if (file == null) file = new Ops.File();
        editing = op;
        editIndex = -1;
        dirty = false;
        overview = false;
        stepEditing = null;
        stepEdit = null;
        editMode = "块";                     // v35：直驱缺省块视图（脚本用例自行 [转脚本]/build）
        scriptSeed = null;
        scriptEdit = null;
        foldedBlocks.clear();                // v31：直驱重置（用例自管）
        selA = -1; selB = -1;
        render();
    }

    /** v35 测试直驱：按列表下标走真实打开路由（含 v8 读源 / 模式推导）。返回错误文案或 null。 */
    public String startOpForTest(int idx) {
        if (file == null || idx < 0 || idx >= file.ops.size()) return "下标无效";
        startEdit(idx);
        return null;
    }

    /** v35 测试读取：当前会话视图（块/脚本）。 */
    public String editModeForTest() { return editMode; }

    /** v35 测试读取：当前脚本编辑页（未建 = null）。 */
    public ScriptEdit scriptEditorForTest() { return scriptEdit; }

    /** 退出编辑/步骤编辑页；返回是否处理了（供返回键）。 */
    public boolean closeEditor() {
        if (selecting()) {                   // v31：连选中返回键 = 退出连选（先于退出编辑）
            clearSel(true);
            return true;
        }
        if (stepEditing != null) {
            stepEditing = null;
            stepEdit = null;
            render();
            restoreScroll();   // v27：返回键退出步骤编辑也恢复原滚动位置
            return true;
        }
        if (editing != null) {
            boolean draft = editIndex < 0;       // 草稿（新建中）：返回键 = 丢弃新建
            exitScriptSession();                 // v35：脚本会话一并收场（[转块] 才是回块视图的正路）
            editing = null;
            render();
            if (draft) Ui.toast("已取消：未创建操作");
            return true;
        }
        return false;
    }

    // ---------------- v31：卡片收起 / 步骤复制 / 连选 ----------------

    /** v31：切换块头收起（卡片视图与流程总览共用一套收起集；按步骤对象身份，编辑/移动不串位）。 */
    public void toggleFoldAt(int headIdx) {
        if (editing == null || headIdx < 0 || headIdx >= editing.steps.size()) return;
        Ops.Step hs = editing.steps.get(headIdx);
        if (hs.type != Ops.T_LOOP && hs.type != Ops.T_IF) return;
        if (!foldedBlocks.remove(hs)) foldedBlocks.add(hs);
        render();
    }

    /** v31：行交互补强——长按进入连选；连选态中点按 = 定范围（覆盖行原有点击）；范围内行加琥珀底。 */
    private void attachRowSel(View row, final int idx) {
        row.setLongClickable(true);
        row.setOnLongClickListener(new View.OnLongClickListener() {
            public boolean onLongClick(View v) { beginSel(idx); return true; }
        });
        if (!selecting()) return;
        row.setClickable(true);
        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { extendSel(idx); }
        });
        int[] r = selRange();
        if (r != null && idx >= r[0] && idx <= r[1]) {
            row.setBackground(Ui.pressable(act, 0x2EE8B14F, 0x55E8B14F, 8));   // 选中行 = 琥珀 18%/33%
        }
    }

    /** 是否处于连选模式。 */
    private boolean selecting() { return selA >= 0 && editing != null; }

    /** 连选锚点（长按步骤行进入；已在连选则重设锚点）。 */
    public void beginSel(int idx) {
        if (editing == null || idx < 0 || idx >= editing.steps.size()) return;
        if (StepClip.normalizeRange(editing.steps, idx, idx) == null) {
            Ui.toast("结构破损（先修复再连选）");
            return;
        }
        selA = idx;
        selB = idx;
        if (!selHintShown) {
            selHintShown = true;
            Ui.toast("连选：再点一行定范围（底栏可 [复制所选]）");
        }
        render();
    }

    /** 连选另一端（点行定范围；块边界自动补全——整块/子条件链随头走；破损段拒绝）。 */
    public void extendSel(int idx) {
        if (!selecting() || idx < 0 || idx >= editing.steps.size()) return;
        if (StepClip.normalizeRange(editing.steps, selA, idx) == null) {
            Ui.toast("结构破损，无法连选此段");
            return;
        }
        selB = idx;
        render();
    }

    /** 连选范围（归一后，含端点；未连选 = null）。 */
    public int[] selRange() {
        if (!selecting()) return null;
        return StepClip.normalizeRange(editing.steps, selA, selB);
    }

    /** 退出连选（renderNow = 是否立即重渲染）。 */
    private void clearSel(boolean renderNow) {
        if (selA < 0 && selB < 0) return;
        selA = -1;
        selB = -1;
        if (renderNow) render();
    }

    /** v31：复制单行单元（块头 = 整块含子条件/否则/块尾；其余 = 单步）。返回错误文案或 null（供 TestBench 直驱）。 */
    public String copyOne(int idx) {
        if (editing == null || idx < 0 || idx >= editing.steps.size()) return "不在操作编辑中";
        int tail = Ops.unitTail(editing.steps, idx);
        if (tail < 0) return "结构未配对（先修复再复制）";
        copyUnit(idx, tail);
        render();                                   // 底栏出现 [粘贴（N 步）]
        return null;
    }

    /** 复制当前连选范围（底栏 [复制所选]）。 */
    public void copySelected() {
        int[] r = selRange();
        if (r == null) { clearSel(true); return; }
        copyUnit(r[0], r[1]);
        clearSel(false);
        render();
    }

    /** 单元 [head..tail] → 剪贴板（深拷贝）+ toast；粘完不清空，可跨操作多处粘。 */
    private void copyUnit(int head, int tail) {
        StepClip.copy(editing.steps, head, tail, editing.name, editing.varSlots());
        Ui.toast("已复制 " + (tail - head + 1) + " 步");
    }

    /** v31：粘贴落点对话框（剪贴板非空时底栏 [粘贴（N 步）] 调用）。 */
    private void pasteDialog() {
        if (!buildPasteItems()) return;
        Forms.pickDialog(act, pasteTitle, pasteLabels, -1, new Forms.Pick() {
            public void onPick(int i) {
                if (i >= 0 && i < pastePositions.size()) pasteClipboardAt(pastePositions.get(i).intValue());
            }
        });
    }

    /** 测试预览共享：粘贴对话框标题 / 条目 / 对应插入下标（buildPasteItems 重填）。 */
    public String pasteTitle = "";
    public final List<String> pasteLabels = new ArrayList<String>();
    public final List<Integer> pastePositions = new ArrayList<Integer>();

    /** 组装粘贴落点条目（快捷项：末尾 / 开头 / 同操作「原位置之后」；其余合法落点升序）。无落点 = false + toast。 */
    public boolean buildPasteItems() {
        pasteLabels.clear();
        pastePositions.clear();
        if (editing == null || StepClip.empty()) return false;
        java.util.List<Integer> pos = StepClip.legalPositions(editing.steps, StepClip.steps);
        if (pos.isEmpty()) {
            if (editing.steps.size() + StepClip.size() > Ops.MAX_STEPS)
                Ui.toast("粘贴将超过上限 " + Ops.MAX_STEPS + "（当前 " + editing.steps.size()
                        + " + 剪贴板 " + StepClip.size() + " 步）");
            else Ui.toast("没有可粘贴的位置（结构限制）");
            return false;
        }
        int n = editing.steps.size();
        java.util.LinkedHashSet<Integer> order = new java.util.LinkedHashSet<Integer>();
        if (pos.contains(Integer.valueOf(n))) order.add(Integer.valueOf(n));          // 末尾（最常用）
        if (pos.contains(Integer.valueOf(0))) order.add(Integer.valueOf(0));          // 开头
        int pSrc = StepClip.srcIdx + 1;
        if (editing.name.equals(StepClip.srcOp) && StepClip.srcIdx >= 0 && pSrc <= n
                && pos.contains(Integer.valueOf(pSrc))) order.add(Integer.valueOf(pSrc));   // 原位置之后（同操作）
        for (Integer p : pos) order.add(p);
        for (Integer p : order) {
            int k = p.intValue();
            String lb;
            if (k == n) lb = "粘贴到末尾（第 " + (n + 1) + " 位）";
            else if (k == 0) lb = "粘贴到第 1 位（开头）";
            else if (editing.name.equals(StepClip.srcOp) && StepClip.srcIdx >= 0 && k == StepClip.srcIdx + 1)
                lb = "粘贴到原位置之后（第 " + (k + 1) + " 位）";
            else lb = "粘贴到第 " + (k + 1) + " 位";
            pasteLabels.add(lb);
            pastePositions.add(p);
        }
        pasteTitle = "粘贴「" + StepClip.label + "」到…";
        return true;
    }

    /** v31：粘贴到插入下标 p（0 基）；成功 = 定位到新行 + toast（含轻预检提醒），失败 = toast 原因。
     *  返回提示文案或错误（供 TestBench 直驱）。 */
    public String pasteClipboardAt(int p) {
        if (editing == null || StepClip.empty()) return "不在操作编辑中";
        int m = StepClip.size();
        int n = editing.steps.size();
        if (p < 0 || p > n) p = n;
        String notice = StepClip.preflightNotice(editing.steps, editing.varSlots(), StepClip.steps, StepClip.srcVars);
        String err = StepClip.paste(editing.steps, p);
        if (err != null) { Ui.toast(err); return err; }
        clearSel(false);
        render();
        focusStep(p);
        String msg = "已粘贴 " + m + " 步 · 第 " + (p + 1) + " 位";
        if (notice != null) msg += "（" + notice + "）";
        Ui.toast(msg);
        return msg;
    }

    // ---------- 写盘 ----------

    /** 测试台直驱用：写盘重定向到别处（正常恒 null = 写 Ops.PATH 并镜像方案）。 */
    public String pathOverride = null;

    /** v22：提交即落盘——校验 → 整文件原子写（+ 镜像方案）→ 「已保存」生效口径反馈。what = 反馈文案。 */
    private void save(final String what) {
        saveEx(what, null);
    }

    /** v35：save + progs 随删（rmProgs 非空 = 这些操作的 .vts/.vtbc 在写盘后清扫——删除操作路径）。 */
    private void saveEx(final String what, final java.util.List<String> rmProgs) {
        if (file == null) { Ui.toast("尚未加载"); return; }
        if (editing != null) { Ui.toast("请先完成或取消当前编辑"); return; }
        for (Ops.Op o : file.ops) {
            String err = Ops.validate(o, file, o);
            if (err != null) { Ui.toast(err); return; }
            String ge = regionRefErr(o);
            if (ge != null) { Ui.toast(ge); return; }
        }
        final String text = file.serialize();
        final String ov = pathOverride;
        final String path = ov != null ? ov : Ops.PATH;
        Ui.bg(new Runnable() {
            public void run() {
                if (ov == null) Su.backup(Ops.PATH);
                final Su.R res = Su.writeAtomic(path, text.getBytes());
                if (res.ok() && rmProgs != null) {
                    StringBuilder cmd = new StringBuilder();
                    for (String nm : rmProgs) {
                        cmd.append("rm -f '").append(OpsV8.srcPath(OpsV8.base(), nm)).append("' '")
                           .append(OpsV8.progPath(OpsV8.base(), nm)).append("'; ");
                    }
                    if (cmd.length() > 0) Su.run(cmd.toString());
                }
                if (res.ok() && ov == null) Schemes.mirror();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (!res.ok()) { Ui.toast("保存失败：" + res.trimmed()); return; }
                        dirty = false;
                        render();
                        Ui.saved(what);
                    }
                });
            }
        }, null);
    }

    // ---------- 视觉素材集成（v7 T8） ----------

    private final StepEdit.Visual visual = new StepEdit.Visual() {
        public void newTmpl(Forms.Chips field) { openTmpl(false, null); }
        public void editTmpl(String name) {
            if (name == null || name.isEmpty()) { Ui.toast("先选一个模板"); return; }
            openTmpl(true, name);
        }
        public void testTmpl(String name, int thresh, boolean cc, String region) {
            if (name == null || name.isEmpty()) { Ui.toast("先选一个模板"); return; }
            openTmplTest(name, thresh, cc, region);
        }
        public void newPts(Forms.Chips field) { openPts(false, null); }
        public void editPts(String name) {
            if (name == null || name.isEmpty()) { Ui.toast("先选一个点集"); return; }
            openPts(true, name);
        }
        public void testPts(String name, String region) {
            if (name == null || name.isEmpty()) { Ui.toast("先选一个点集"); return; }
            openPtsTest(name, region);
        }
        public void pickPointXY(final Forms.Num fx, final Forms.Num fy) {
            VisualPage.Session s = new VisualPage.Session();
            s.mode = VisualPage.MODE_PICK_POINT;
            VisualPage.open(s, new VisualPage.Done() {
                public void onTmplSaved(String name) { }
                public void onPointPicked(int lx, int ly) {
                    if (fx != null) { fx.setValue(lx); fx.clearError(); }
                    if (fy != null) { fy.setValue(ly); fy.clearError(); }
                }
                public void onColorPicked(int rgb) { }
                public void onCanceled() { }
            });
        }
        public void pickColor(final StepEdit.ColorSink sink) {
            VisualPage.Session s = new VisualPage.Session();
            s.mode = VisualPage.MODE_PICK_COLOR;
            VisualPage.open(s, new VisualPage.Done() {
                public void onTmplSaved(String name) { }
                public void onPointPicked(int lx, int ly) { }
                public void onColorPicked(int rgb) { if (sink != null) sink.onColor(rgb); }
                public void onCanceled() { }
            });
        }
    };

    private void openTmpl(boolean edit, String name) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = edit ? VisualPage.MODE_TMPL_EDIT : VisualPage.MODE_TMPL_NEW;
        s.name = name;
        VisualPage.open(s, new VisualPage.Done() {
            public void onTmplSaved(String n) {
                if (stepEditing != null) stepEditing.ref = n;
                reloadNamesAndRerender();
            }
            public void onPointPicked(int lx, int ly) { }
            public void onColorPicked(int rgb) { }
            public void onCanceled() { }
        });
    }

    private void openTmplTest(String name, int thresh, boolean cc, String region) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = VisualPage.MODE_TMPL_TEST;
        s.name = name;
        s.thresh = thresh;
        s.colorCheck = cc;
        s.region = region;
        s.regionRect = regionRect(region);
        VisualPage.open(s, null);
    }

    private void openPts(boolean edit, String name) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = edit ? VisualPage.MODE_PTS_EDIT : VisualPage.MODE_PTS_NEW;
        s.name = name;
        VisualPage.open(s, new VisualPage.Done() {
            public void onTmplSaved(String n) {
                if (stepEditing != null) stepEditing.ref = n;
                reloadNamesAndRerender();
            }
            public void onPointPicked(int lx, int ly) { }
            public void onColorPicked(int rgb) { }
            public void onCanceled() { }
        });
    }

    private void openPtsTest(String name, String region) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = VisualPage.MODE_PTS_TEST;
        s.name = name;
        s.region = region;
        s.regionRect = regionRect(region);
        VisualPage.open(s, null);
    }

    /** 区域几何（rect= x,y,w,h；circle= 外接矩形）→ 逻辑矩形（区域限定试验用）。 */
    private int[] regionRect(String regionId) {
        if (regionId == null || regionId.isEmpty()) return null;
        for (Regions.Region r : regionsAll) {
            if (r.id.equals(regionId)) {
                if (r.type == 0) return new int[]{r.a1, r.a2, r.a3, r.a4};
                return new int[]{r.a1 - r.a3, r.a2 - r.a3, r.a3 * 2, r.a3 * 2};
            }
        }
        return null;
    }

    /** 素材保存后：只重载名单并重建当前步编辑（不打断操作编辑状态）。 */
    private void reloadNamesAndRerender() {
        Ui.bg(new Runnable() {
            public void run() {
                final List<String> tmpls = new ArrayList<String>();
                final List<String> ptss = new ArrayList<String>();
                Su.R tl = Su.run("ls " + Ops.TMPL_DIR + " 2>/dev/null");
                if (tl.ok() && tl.out != null) {
                    for (String ln : tl.out.split("\n")) {
                        String fn = ln.trim();
                        if (fn.endsWith(".tmpl")) tmpls.add(fn.substring(0, fn.length() - 5));
                        else if (fn.endsWith(".pts")) ptss.add(fn.substring(0, fn.length() - 4));
                    }
                }
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        tmplNames = tmpls;
                        ptsNames = ptss;
                        if (stepEditing != null) renderStepEditor();
                    }
                });
            }
        }, null);
    }
}
