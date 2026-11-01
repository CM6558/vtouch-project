package com.vtouch.editor;

import android.content.Context;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** 区域页：regions.conf 编辑（列表 ↔ 全屏编辑页）。交互反馈口径：增/改/删都有 toast 确认。 */
public class RegionsPage {
    private final Context act;
    private LinearLayout bar, container;
    private TextView statusLine;
    private Regions.File file;
    private List<String> opNames = new ArrayList<String>();
    private boolean dirty = false;
    private Regions.Region editing = null;   // 编辑中的副本（null = 列表模式）
    private int editingIndex = -1;           // -1 = 新建
    private LinearLayout projRow;            // v12c：总览调整入口行（仅列表模式显示）

    public RegionsPage(Context a) { act = a; }

    public View build() {
        LinearLayout root = Ui.col(act);
        bar = Ui.row(act);
        bar.setPadding(Ui.dp(act, 16), Ui.dp(act, 10), Ui.dp(act, 8), Ui.dp(act, 6));
        statusLine = Ui.mono(act, "", 12, Ui.DIM);
        statusLine.setSingleLine(true);
        statusLine.setEllipsize(android.text.TextUtils.TruncateAt.END);
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(bar);
        projRow = Ui.row(act);
        projRow.setPadding(Ui.dp(act, 12), 0, Ui.dp(act, 12), Ui.dp(act, 6));
        root.addView(projRow);
        ScrollView sc = new ScrollView(act);
        container = Ui.col(act);
        container.setPadding(Ui.dp(act, 8), Ui.dp(act, 4), Ui.dp(act, 8), Ui.dp(act, 16));
        sc.addView(container);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    public void refresh() {
        Ui.bg(new Runnable() {
            public void run() {
                final String text = Su.read(Regions.PATH);
                final List<String> ops = new ArrayList<String>();
                String ot = Su.read(Ops.PATH);
                if (ot != null) {
                    try {
                        Ops.File of = Ops.File.parse(ot);
                        for (Ops.Op o : of.ops) ops.add(o.name);
                    } catch (Exception e) { /* 忽略 */ }
                }
                Ui.MAIN.post(new Runnable() {
                    public void run() { applyLoad(text, ops); }
                });
            }
        }, null);
    }

    private void applyLoad(String text, List<String> ops) {
        opNames = ops;
        /* v26：不再重置 editing/editingIndex——切页/刷新不打断正在编辑的内容（会话保留） */
        if (text == null) {
            statusLine.setText("读取失败：regions.conf 不存在或 root 不可用");
            if (editing == null) container.removeAllViews(); else render();
            return;
        }
        try {
            file = Regions.File.parse(text);
            dirty = false;
            render();
        } catch (Exception e) {
            statusLine.setText("解析失败：" + e.getMessage());
            container.removeAllViews();
        }
    }

    /** #frame <rot> <w> <h> → 显示成「屏帧 w×h」。 */
    private String frameText() {
        if (file == null || file.frameLine == null) return "";
        String[] p = file.frameLine.trim().split("\\s+");
        if (p.length >= 4 && "#frame".equals(p[0])) return " · 屏帧 " + p[2] + "×" + p[3];
        return " · " + file.frameLine;
    }

    /** 编辑页的位置提示（带坐标范围）。 */
    private String frameHint() {
        if (file == null || file.frameLine == null) return "";
        String[] p = file.frameLine.trim().split("\\s+");
        if (p.length >= 4 && "#frame".equals(p[0])) {
            try {
                int w = Integer.parseInt(p[2]);
                int h = Integer.parseInt(p[3]);
                return "屏帧 " + w + "×" + h + " · 坐标 0.." + (w - 1) + " / 0.." + (h - 1);
            } catch (Exception e) { /* 忽略 */ }
        }
        return "";
    }

    /** v12c/d：全部区域总览（看图器 MODE_REGIONS：同屏、逐个拖调、可拖画新建；返回后重读盘刷新列表）。
     *  arm=true = 进入即置「新矩形」画具；focus=进入即选中该 id。 */
    private void openRegionCanvas(boolean arm, String focus) {
        if (Ui.HOST == null) return;
        if (file == null) { Ui.toast("读取失败：regions.conf 不存在或 root 不可用"); return; }
        VisualPage.Session vs = new VisualPage.Session();
        vs.mode = VisualPage.MODE_REGIONS;
        vs.armNew = arm ? 1 : 0;
        vs.focusId = focus;
        if (arm) Ui.toast("拖动画面 = 画一个新矩形 · [＋ 圆] 画圆 · 改动自动落盘");
        VisualPage.open(vs, new VisualPage.Done() {
            public void onTmplSaved(String name) { }
            public void onPointPicked(int lx, int ly) { }
            public void onColorPicked(int rgb) { }
            public void onCanceled() { refresh(); }
        });
    }

    private void render() {
        if (file == null) return;
        if (editing == null) renderList();
        else renderEditor();
    }

    // ---------- 列表模式 ----------

    private void renderList() {
        bar.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        projRow.removeAllViews();
        TextView adjustAll = Ui.btnSmall(act, "打开画布（增删改）");
        adjustAll.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { openRegionCanvas(false, null); }
        });
        projRow.addView(adjustAll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        /* v13.1：列表非空时只留「打开画布」一个入口（＋新建 = 画布内 ＋矩形/＋圆，不再重复按钮）；
         * 空态走 emptyState 的「＋ 新建区域（在画布上拖画）」。
         * v22：列表 [保存] 已删——区域表单保存/删除均即时落盘，无待保存态。 */
        projRow.setVisibility(file.regions.isEmpty() ? View.GONE : View.VISIBLE);

        container.removeAllViews();
        statusLine.setText("共 " + file.regions.size() + " 个区域" + frameText());
        if (file.regions.isEmpty()) {
            container.addView(Ui.emptyState(act, "还没有区域",
                    "区域是悬浮窗里可点击 / 可拖动的矩形或圆形，可绑定操作。",
                    "＋ 新建区域（在画布上拖画）", new View.OnClickListener() {
                        public void onClick(View v) { openRegionCanvas(true, null); }
                    }));
            return;
        }
        for (final Regions.Region r : file.regions) {
            LinearLayout card = Ui.card(act);
            card.setBackground(Ui.pressableStroked(act, Ui.CARD, Ui.CARD_LINE, 0xFF23272C, 8));
            card.setClickable(true);
            if (r.en == 0) card.setAlpha(0.62f);   // 停用区域整卡弱化（状态一眼可见）
            card.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { startEdit(r); }
            });
            LinearLayout titleRow = Ui.row(act);
            titleRow.addView(Ui.monoBold(act, r.id, 15, Ui.TEXT),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView edit = Ui.btnFlat(act, "✎ 编辑", Ui.ACCENT);
            edit.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { startEdit(r); }
            });
            TextView cp = Ui.btnFlat(act, "复制到…", Ui.DIM);
            cp.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { CrossCopy.copyFlow(act, true, r.id, dirty); }
            });
            TextView del = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
            del.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { deleteConfirm(r); }
            });
            titleRow.addView(edit);
            titleRow.addView(cp);
            titleRow.addView(del);
            card.addView(titleRow);
            LinearLayout metaRow = Ui.row(act);
            metaRow.setPadding(0, Ui.dp(act, 4), 0, 0);
            metaRow.addView(Ui.chip(act, r.typeName(), Ui.DIM));
            if (r.kind == 1) metaRow.addView(Ui.chip(act, "开关型", Ui.ACCENT));
            if (r.en == 0) metaRow.addView(Ui.chip(act, "停用", Ui.WARN));
            card.addView(metaRow);
            TextView geom = Ui.mono(act, r.geomText(), 13, Ui.DIM);
            geom.setPadding(0, Ui.dp(act, 6), 0, 0);
            card.addView(geom);
            if (!"-".equals(r.bindOp)) {
                LinearLayout bindRow = Ui.row(act);
                bindRow.setPadding(0, Ui.dp(act, 6), 0, 0);
                bindRow.addView(Ui.chip(act, r.bindOp + " · "
                        + ("press".equals(r.bindEv) ? "完整按压" : "按下"), Ui.OK));
                card.addView(bindRow);
            }
            container.addView(card);
        }
    }

    // ---------- 编辑模式（全屏表单） ----------

    private void startEdit(Regions.Region r) {
        editing = (r == null) ? new Regions.Region("r" + (file.regions.size() + 1)) : r.copy();
        editingIndex = (r == null) ? -1 : file.regions.indexOf(r);
        render();
    }

    private void renderEditor() {
        final Regions.Region eo = editing;
        bar.removeAllViews();
        projRow.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        statusLine.setText(editingIndex < 0 ? "新建区域" : "编辑区域 " + eo.id);
        TextView cancelB = Ui.btnSmall(act, "取消");
        TextView saveB = Ui.btnSmallPrimary(act, "保存");
        bar.addView(cancelB);
        bar.addView(saveB);

        container.removeAllViews();
        final RegionEdit edit = RegionEdit.build(act, eo, opNames, frameHint(), new Runnable() {
            public void run() { openRegionCanvas(false, eo.id); }   /* v12d：位置调整统一走画布（聚焦本区域） */
        });
        container.addView(edit.root);

        if (editingIndex >= 0) {
            TextView delB = Ui.btnSmallTonal(act, "删除此区域", Ui.WARN);
            LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            dlp.setMargins(Ui.dp(act, 12), Ui.dp(act, 6), Ui.dp(act, 12), Ui.dp(act, 4));
            delB.setLayoutParams(dlp);
            delB.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { deleteFromEditor(); }
            });
            container.addView(delB);
        }

        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                editing = null;
                render();
            }
        });
        saveB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String err = edit.collect(eo);
                if (err != null) { Ui.toast(err); return; }
                Regions.Region self = editingIndex >= 0 ? file.regions.get(editingIndex) : null;
                err = Regions.validate(eo, file, self);
                if (err != null) { Ui.toast(err); return; }
                final String what;
                if (self == null) {
                    file.regions.add(eo);
                    what = "区域「" + eo.id + "」已创建";
                } else {
                    self.id = eo.id;
                    self.type = eo.type;
                    self.a1 = eo.a1; self.a2 = eo.a2; self.a3 = eo.a3; self.a4 = eo.a4;
                    self.en = eo.en; self.bindOp = eo.bindOp; self.bindEv = eo.bindEv; self.kind = eo.kind;
                    what = "区域「" + eo.id + "」已更新";
                }
                dirty = true;
                editing = null;
                render();
                save(what);   /* v22：提交即落盘（不再等列表 [保存]） */
            }
        });
    }

    private void deleteFromEditor() {
        final Regions.Region target = file.regions.get(editingIndex);
        final Ui.Dlg d = Ui.dialog(act, "删除区域 " + target.id + "？");
        d.body.addView(Ui.dlgMessage(act, "引用它的操作绑定 / 门控会变为悬空（核心按未命中处理）。"));
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView ok = Ui.btnSmallTonal(act, "删除", Ui.WARN);
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                file.regions.remove(target);
                editing = null;
                render();
                d.dismiss();
                save("已删除区域「" + target.id + "」");  /* v13：删除立即写盘 */
            }
        });
        d.actions.addView(cancel);
        d.actions.addView(ok);
        d.show();
    }

    private void deleteConfirm(final Regions.Region r) {
        final Ui.Dlg d = Ui.dialog(act, "删除区域 " + r.id + "？");
        d.body.addView(Ui.dlgMessage(act, "引用它的操作绑定 / 门控会变为悬空（核心按未命中处理）。"));
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView ok = Ui.btnSmallTonal(act, "删除", Ui.WARN);
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                file.regions.remove(r);
                render();
                d.dismiss();
                save("已删除区域「" + r.id + "」");      /* v13：删除立即写盘 */
            }
        });
        d.actions.addView(cancel);
        d.actions.addView(ok);
        d.show();
    }

    /** 退出编辑页；返回是否处理了（供返回键）。 */
    public boolean closeEditor() {
        if (editing == null) return false;
        editing = null;
        render();
        return true;
    }

    /** v26：是否有进行中的编辑会话（Shell 切页/刷新据此保留现场，不重载）。 */
    public boolean busy() { return editing != null; }

    /** v22：提交/删除即落盘——整文件原子写 + 镜像；what = 反馈文案（走 Ui.saved 生效口径）。 */
    private void save(final String what) {
        if (file == null) { Ui.toast("尚未加载"); return; }
        for (Regions.Region r : file.regions) {
            String err = Regions.validate(r, file, r);
            if (err != null) { Ui.toast(err); return; }
        }
        final String text = file.serialize();
        Ui.bg(new Runnable() {
            public void run() {
                Su.backup(Regions.PATH);
                final Su.R res = Schemes.publishConfig(Regions.PATH, text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
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
}
