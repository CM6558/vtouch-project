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
 */
public class OpsPage {
    private final Context act;
    private LinearLayout bar, container, bottomBar;
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
    private int stepIndex = -1;            // -1 = 新增步骤
    private int insertBefore = -1;         // v12：块内插入的目标位置（-1 = 末尾）
    private String insertCtx = "";         // v12.2：插入落点语境（成立分支 / 否则分支 / 循环体；进插入提示）
    private int presetType = 0;            // v12.1：弹层选定的类型（用后清零 = 点按）
    private boolean overview = false;      // 步骤流：总览模式（紧凑时间线）
    private final java.util.Set<Ops.Step> foldedBlocks =     // v12.3：总览已收起的块（按步骤对象身份；编辑后不串位）
            java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<Ops.Step, Boolean>());

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
        ScrollView sc = new ScrollView(act);
        container = Ui.col(act);
        container.setPadding(Ui.dp(act, 8), Ui.dp(act, 4), Ui.dp(act, 8), Ui.dp(act, 16));
        sc.addView(container);
        root.addView(sc, new LinearLayout.LayoutParams(
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
                Ui.MAIN.post(new Runnable() {
                    public void run() { applyLoad(text, ids, sw, tmpls, ptss, rAll); }
                });
            }
        }, null);
    }

    private void applyLoad(String text, List<String> ids, List<String> sw,
                           List<String> tmpls, List<String> ptss, List<Regions.Region> rAll) {
        regionIds = ids;
        switchIds = sw;
        regionsAll = rAll != null ? rAll : new ArrayList<Regions.Region>();
        tmplNames = tmpls != null ? tmpls : new ArrayList<String>();
        ptsNames = ptss != null ? ptss : new ArrayList<String>();
        if (text == null) {
            statusLine.setText("读取失败：ops.conf 不存在或 root 不可用");
            container.removeAllViews();
            return;
        }
        try {
            file = Ops.File.parse(text);
            dirty = false;
            editing = null;
            stepEditing = null;
            render();
        } catch (Exception e) {
            statusLine.setText("解析失败：" + e.getMessage());
            container.removeAllViews();
        }
    }

    private void render() {
        if (file == null) return;
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
        TextView add = Ui.btnSmall(act, "＋ 新建操作");
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
            metaRow.addView(Ui.chip(act, o.steps.size() + " 步", Ui.DIM));
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
        editing = file.ops.get(idx).copy();
        editIndex = idx;
        foldedBlocks.clear();               // v12.3：收起状态随编辑会话重置
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
        foldedBlocks.clear();
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
                save("已删除操作「" + o.name + "」");   /* v22：删除即落盘 */
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
        TextView done = Ui.btnSmallPrimary(act, "完成");
        done.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = commitEdit();
                if (err != null) Ui.toast(err);
            }
        });
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
                public void onToggle(int headIdx) {
                    if (headIdx < 0 || headIdx >= editing.steps.size()) return;
                    Ops.Step hs = editing.steps.get(headIdx);
                    if (!foldedBlocks.remove(hs)) foldedBlocks.add(hs);
                    render();
                }
            }));
        } else {
        // v12：块感知渲染（形态 C 色带块风）——块体嵌色带壳；块内行平铺、块外行卡片
        java.util.ArrayList<LinearLayout> bodyStack = new java.util.ArrayList<LinearLayout>();
        java.util.ArrayList<LinearLayout> parentStack = new java.util.ArrayList<LinearLayout>();
        LinearLayout listHost = container;
        for (int i = 0; i < eo.steps.size(); i++) {
            final int idx = i;
            final Ops.Step s = eo.steps.get(i);
            if (s.type == Ops.T_LOOP || s.type == Ops.T_IF) {
                LinearLayout shell = Ui.col(act);
                shell.setBackground(Ui.blockBg(act,
                        s.type == Ops.T_LOOP ? Ui.ACCENT : Ui.COND_BAR,
                        s.type == Ops.T_LOOP ? Ui.LOOP_TINT : Ui.COND_TINT));
                listHost.addView(shell);
                shell.addView(blockHeadRow(idx, s, bodyStack.size()));       // v12.3：代码式缩进
                bodyStack.add(shell);
                parentStack.add(listHost);
                listHost = shell;
            } else if (s.type == Ops.T_ENDLOOP || s.type == Ops.T_ENDIF) {
                listHost.addView(blockTailRow(idx, s, Math.max(0, bodyStack.size() - 1)));
                if (!bodyStack.isEmpty()) bodyStack.remove(bodyStack.size() - 1);
                if (!parentStack.isEmpty()) listHost = parentStack.remove(parentStack.size() - 1);
            } else if (s.type == Ops.T_ELSE) {
                listHost.addView(elseRow(idx, s, Math.max(0, bodyStack.size() - 1)));   // v12.2：分支分隔行（＋ = 否则分支末尾插入）
            } else {
                listHost.addView(stepRow(idx, s, !bodyStack.isEmpty(), bodyStack.size()));
            }
        }
        }

        // 底部常驻操作条：步数 + 添加（不随滚动，步数多时也随手可加）
        bottomBar.removeAllViews();
        bottomBar.setVisibility(View.VISIBLE);
        bottomBar.addView(Ui.divider(act));
        LinearLayout foot = Ui.row(act);
        foot.setGravity(Gravity.CENTER_VERTICAL);
        foot.setPadding(Ui.dp(act, 14), Ui.dp(act, 8), Ui.dp(act, 12), Ui.dp(act, 8));
        foot.addView(Ui.mono(act, "共 " + eo.steps.size() + " 步（上限 " + Ops.MAX_STEPS + "）", 12, Ui.DIM),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView addStep = Ui.btnSmallPrimary(act, "＋ 添加步骤");
        addStep.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (editing.steps.size() >= Ops.MAX_STEPS) { Ui.toast("步骤数已达上限 " + Ops.MAX_STEPS); return; }
                askTypeThenAdd(-1, "");
            }
        });
        foot.addView(addStep);
        bottomBar.addView(foot);
    }

    /** v12.3：代码式缩进（14dp/级）：块头 / 否则 / 块尾 = 本层；块内步骤 = 下一层（口径同流程总览）。 */
    private int ind(int dep) { return Ui.dp(act, 12 + dep * 14); }

    /** 进入步骤编辑页（idx ≥ 0 编辑已有步骤；-1 新增）。 */
    private void openStep(int idx) {
        stepEditing = (idx >= 0) ? editing.steps.get(idx).copy() : newStep(presetType);
        presetType = 0;   // 一次性（弹层选定后回默认）
        stepIndex = idx;
        render();
    }

    private Ops.Step newStep(int type) {
        Ops.Step s = new Ops.Step();
        s.type = type;
        if (type == Ops.T_TAP) s.ms = 50;
        if (type == Ops.T_LOOP) { s.a1 = 0; s.a2 = 3; }   // 默认：循环 3 次
        return s;
    }

    /** v12.1：先底部弹层选类型 → 再进步骤编辑页（插入位置 insertAt，-1 = 末尾；ctx = 落点语境，插入成功后提示）。 */
    private void askTypeThenAdd(final int insertAt, final String ctx) {
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
        final StepEdit edit = StepEdit.build(act, se, regionIds, switchIds,
                tmplNames, ptsNames, editing.varSlots(), visual);
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
                    render();
                }
            });
            container.addView(delB);
        }

        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                stepEditing = null;
                insertBefore = -1;
                insertCtx = "";
                render();
            }
        });
        saveB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = edit.collect();
                if (err != null) { Ui.toast(err); return; }
                if (stepIndex >= 0) {
                    editing.steps.set(stepIndex, se);
                    Ui.toast("步骤 " + (stepIndex + 1) + " 已更新");
                } else {
                    int at = (insertBefore >= 0 && insertBefore <= editing.steps.size())
                            ? insertBefore : editing.steps.size();
                    /* v12.2：结构头自动带出配套（12→+13；14→+15+16）——插入统一走 Ops.insertStep（位置由块导航先算好） */
                    if (!Ops.insertStep(editing.steps, at, se)) {
                        Ui.toast("步骤数将超过上限 " + Ops.MAX_STEPS + "（结构行按配套计）");
                        return;
                    }
                    String suf = insertCtx.isEmpty() ? "" : " · " + insertCtx;
                    if (se.type == Ops.T_LOOP) Ui.toast("已插入循环（含循环结束）" + suf);
                    else if (se.type == Ops.T_IF) Ui.toast("已插入条件块（含否则 / 结束）" + suf);
                    else Ui.toast("已插入步骤 " + (at + 1) + suf);
                }
                insertBefore = -1;
                insertCtx = "";
                stepEditing = null;
                render();
            }
        });
    }

    /** 一行步骤（v12）：块外 = 卡片；块内 = 平铺行（色带壳内）。dep = 代码式缩进层级（v12.3）。 */
    private LinearLayout stepRow(final int idx, final Ops.Step s, final boolean inBlock, final int dep) {
        LinearLayout sc = inBlock ? Ui.col(act) : Ui.card(act);
        if (!inBlock && s.known()) {
            sc.setBackground(Ui.pressableStroked(act, Ui.CARD, Ui.CARD_LINE, 0xFF23272C, 8));
        }
        if (inBlock) {
            sc.setPadding(ind(dep), Ui.dp(act, 6), Ui.dp(act, 10), Ui.dp(act, 6));
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
        TextView sum = Ui.tv(act, s.summary(), inBlock ? 12.5f : 13, Ui.DIM, false);
        sum.setPadding(0, Ui.dp(act, 3), 0, 0);
        sc.addView(sum);
        LinearLayout btns = Ui.row(act);
        btns.setGravity(Gravity.END);
        if (s.known()) {
            TextView edit = Ui.btnFlat(act, "✎ 编辑", Ui.ACCENT);
            edit.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { openStep(idx); }
            });
            btns.addView(edit);
        } else {
            btns.addView(Ui.tv(act, "类型未解锁（可移动/删除）", 11, Ui.WARN, false));
        }
        TextView mv = Ui.btnFlat(act, "↕ 移动", Ui.ACCENT);
        mv.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { moveStepDialog(idx); }
        });
        TextView del = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
        del.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                editing.steps.remove(idx);
                render();
                Ui.toast("已删除步骤 " + (idx + 1));
            }
        });
        btns.addView(mv);
        btns.addView(del);
        sc.addView(btns);
        return sc;
    }

    /** 块头行（v12/v12.2）：类型 + 参数摘要（琥珀粗体）；点行=编辑块属性。dep = 代码式缩进层级（v12.3）。
     *  右侧 [＋] = 在本块体内末尾插入：条件块 → 成立分支末尾（否则之前）；循环 → 循环体末尾（循环结束之前）。 */
    private LinearLayout blockHeadRow(final int idx, final Ops.Step s, final int dep) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(ind(dep), Ui.dp(act, 7), Ui.dp(act, 8), Ui.dp(act, 7));
        row.setBackground(Ui.pressable(act, 0x00000000, 0x2EFFFFFF, 8));
        row.setClickable(true);
        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { openStep(idx); }
        });
        LinearLayout col = Ui.col(act);
        col.addView(Ui.tv(act, (idx + 1) + ". " + Ops.typeName(s.type), 14, Ui.ACCENT, true));
        TextView sum = Ui.tv(act, s.summary(), 12.5f, Ui.DIM, false);
        sum.setPadding(0, Ui.dp(act, 2), 0, 0);
        col.addView(sum);
        row.addView(col, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView add = Ui.btnFlat(act, "＋", Ui.ACCENT);
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                int at = (s.type == Ops.T_IF)
                        ? Ops.ifTrueTail(editing.steps, idx) : Ops.pairTail(editing.steps, idx);
                if (at < 0) { Ui.toast("结构未配对（先修复再插入）"); return; }
                if (editing.steps.size() >= Ops.MAX_STEPS) { Ui.toast("步骤数已达上限 " + Ops.MAX_STEPS); return; }
                askTypeThenAdd(at, s.type == Ops.T_IF ? "成立分支" : "循环体");
            }
        });
        row.addView(add);
        return row;
    }

    /** 块尾行（v12）：灰小字（循环结束 / 条件块结束）+ [＋ 否则]（条件块无否则时）+ [✕ 整块]（两步确认）。 */
    private LinearLayout blockTailRow(final int idx, final Ops.Step s, final int dep) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(ind(dep), Ui.dp(act, 3), Ui.dp(act, 8), Ui.dp(act, 5));
        row.addView(Ui.tv(act, Ops.typeName(s.type), 11.5f, Ui.FAINT, false),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        if (s.type == Ops.T_ENDIF && !hasElse(idx)) {
            TextView addElse = Ui.btnFlat(act, "＋ 否则", Ui.DIM);
            addElse.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    Ops.Step nl = new Ops.Step();
                    nl.type = Ops.T_ELSE;
                    editing.steps.add(idx, nl);          // 插到条件块结束之前
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
        return row;
    }

    /** 「否则」分隔行（v12.2）：结构行（不入步骤编辑）+ [＋]（否则分支末尾插入）+ [✕ 否则]（两步确认，删标记本身）。
     *  删除只移除结构行「否则」；其下步骤随之归入成立分支一侧（沿原有单行删除语义，未改）。 */
    private LinearLayout elseRow(final int idx, final Ops.Step s, final int dep) {
        LinearLayout row = Ui.row(act);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(ind(dep), Ui.dp(act, 5), Ui.dp(act, 8), Ui.dp(act, 5));
        row.addView(Ui.tv(act, (idx + 1) + ". 否则", 12.5f, Ui.DIM, true));   // 带步号：与流程总览一致，防「跳号」误读
        View line = new View(act);
        line.setBackgroundColor((Ui.COND_BAR & 0x00FFFFFF) | 0x59000000);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(0, Ui.dp(act, 1), 1f);
        llp.leftMargin = Ui.dp(act, 10);
        llp.rightMargin = Ui.dp(act, 6);
        line.setLayoutParams(llp);
        row.addView(line);
        TextView add = Ui.btnFlat(act, "＋", Ui.ACCENT);
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                int head = Ops.blockHeadOf(editing.steps, idx);
                int at = head >= 0 ? Ops.pairTail(editing.steps, head) : -1;
                if (at < 0) { Ui.toast("结构未配对（先修复再插入）"); return; }
                if (editing.steps.size() >= Ops.MAX_STEPS) { Ui.toast("步骤数已达上限 " + Ops.MAX_STEPS); return; }
                askTypeThenAdd(at, "否则分支");
            }
        });
        row.addView(add);
        final TextView del = Ui.btnFlat(act, "✕ 否则", Ui.FAINT);
        del.setOnClickListener(new View.OnClickListener() {
            private boolean armed = false;
            public void onClick(View v) {
                if (!armed) { armed = true; del.setText("确认删除"); del.setTextColor(Ui.WARN); return; }
                editing.steps.remove(idx);
                render();
                Ui.toast("已删除「否则」（其下步骤并入成立分支）");
            }
        });
        row.addView(del);
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
        render();
        Ui.toast("已删除整块（" + n + " 步）");
    }

    /** 「移动到第 N 位」对话框：开头/末尾显式快捷 + 中间位号任选（一步到位，替代旧 ↑/↓ 逐步交换）。 */
    private void moveStepDialog(final int idx) {
        final int n = editing.steps.size();
        if (n <= 1) { Ui.toast("只有一步，无需移动"); return; }
        final Ops.Step s = editing.steps.get(idx);
        final List<String> items = new ArrayList<String>();
        final int[] targets = new int[n - 1];
        int p = 0;
        for (int k = 1; k <= n; k++) {
            if (k == idx + 1) continue;   // 跳过当前位置
            if (k == 1) items.add("移到第 1 位（开头）");
            else if (k == n) items.add("移到第 " + n + " 位（末尾）");
            else items.add("移到第 " + k + " 位");
            targets[p++] = k;
        }
        Forms.pickDialog(act, "移动「" + (idx + 1) + ". " + Ops.typeName(s.type) + "」到…",
                items, -1, new Forms.Pick() {
                    public void onPick(int index) {
                        int k = targets[index];
                        Ops.Step mv = editing.steps.remove(idx);
                        editing.steps.add(k - 1, mv);
                        render();
                        Ui.toast("已移到第 " + k + " 位");
                    }
                });
    }

    /** [完成]：校验并提交（草稿 editIndex<0 = 创建、落回列表；已有的 = 更新）。返回错误文案或 null（供 TestBench 直驱）。 */
    public String commitEdit() {
        final boolean isNew = editIndex < 0;
        String err = Ops.validate(editing, file, isNew ? null : file.ops.get(editIndex));
        if (err != null) return err;
        String ge = regionRefErr(editing);
        if (ge != null) return ge;
        final String nm = editing.name;
        if (isNew) file.ops.add(editing);
        else file.ops.set(editIndex, editing);
        dirty = true;
        editing = null;
        editIndex = -1;
        render();
        save(isNew ? "操作「" + nm + "」已创建" : "操作「" + nm + "」已更新");   /* v22：提交即落盘 */
        return null;
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
        render();
    }

    /** 退出编辑/步骤编辑页；返回是否处理了（供返回键）。 */
    public boolean closeEditor() {
        if (stepEditing != null) {
            stepEditing = null;
            render();
            return true;
        }
        if (editing != null) {
            boolean draft = editIndex < 0;       // 草稿（新建中）：返回键 = 丢弃新建
            editing = null;
            render();
            if (draft) Ui.toast("已取消：未创建操作");
            return true;
        }
        return false;
    }

    // ---------- 写盘 ----------

    /** 测试台直驱用：写盘重定向到别处（正常恒 null = 写 Ops.PATH 并镜像方案）。 */
    public String pathOverride = null;

    /** v22：提交即落盘——校验 → 整文件原子写（+ 镜像方案）→ 「已保存」生效口径反馈。what = 反馈文案。 */
    private void save(final String what) {
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
