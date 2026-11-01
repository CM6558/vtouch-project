package com.vtouch.editor;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;

/**
 * 素材页：模板 / 点集 / 帧库 三段。行 = 缩略图 + 名 + 元数据 + 引用数；
 * [预览][编辑][试一下][改名][删除]；[＋ 新建] / 帧库段 [去抓帧]。
 * 删除/改名带引用检查（被哪些操作引用，仅提示不阻断——与面板"悬空按未命中处理"同口径）。
 * 注意：本包自定义了 ImageView（看图器）——此文件内一律用 android.widget.ImageView 全名。
 */
public class AssetPage {
    private final Context act;
    private LinearLayout bar, container;
    private TextView statusLine;
    private int seg = 0;                       // 0 模板 / 1 点集 / 2 帧库
    private volatile boolean refreshing;

    public AssetPage(Context a) { act = a; }

    public View build() {
        LinearLayout root = Ui.col(act);
        bar = Ui.row(act);
        bar.setPadding(Ui.dp(act, 16), Ui.dp(act, 10), Ui.dp(act, 8), Ui.dp(act, 6));
        statusLine = Ui.mono(act, "素材 …", 12, Ui.DIM);
        statusLine.setSingleLine(true);
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(bar);
        ScrollView sc = new ScrollView(act);
        container = Ui.col(act);
        container.setPadding(Ui.dp(act, 8), Ui.dp(act, 4), Ui.dp(act, 8), Ui.dp(act, 16));
        sc.addView(container);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    public void refresh() {
        if (refreshing) return;
        refreshing = true;
        Ui.bg(new Runnable() {
            public void run() {
                final List<String> tmpls = Assets.namesTmpl();
                final List<String> ptss = Assets.namesPts();
                final List<FrameStore.Entry> frames = FrameStore.list();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        refreshing = false;
                        render(tmpls, ptss, frames);
                    }
                });
            }
        }, null);
    }

    private void render(final List<String> tmpls, final List<String> ptss, final List<FrameStore.Entry> frames) {
        bar.removeAllViews();
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        statusLine.setText("模板 " + tmpls.size() + " · 点集 " + ptss.size() + " · 帧 " + frames.size());
        TextView addB = Ui.btnSmall(act, seg == 2 ? "去抓帧" : "＋ 新建");
        addB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (seg == 2) {
                    CaptureChip.start(act, true);
                    try {
                        if (act instanceof android.app.Activity) ((android.app.Activity) act).moveTaskToBack(true);
                    } catch (Throwable ig) { }
                } else {
                    openNew(seg == 1);
                }
            }
        });
        bar.addView(addB);
        if (seg == 2 && !frames.isEmpty()) {
            TextView clr = Ui.btnSmall(act, "清空");
            clr.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    final Ui.Dlg d = Ui.dialog(act, "清空帧库？");
                    d.body.addView(Ui.dlgMessage(act, "将删除全部 " + frames.size() + " 帧（不可恢复）。"));
                    TextView no = Ui.btnFlat(act, "取消", Ui.DIM);
                    no.setOnClickListener(new View.OnClickListener() {
                        public void onClick(View v) { d.dismiss(); }
                    });
                    TextView yes = Ui.btnSmallTonal(act, "清空", Ui.WARN);
                    yes.setOnClickListener(new View.OnClickListener() {
                        public void onClick(View v) {
                            d.dismiss();
                            FrameStore.clearAll();
                            refresh();
                        }
                    });
                    d.actions.addView(no);
                    d.actions.addView(yes);
                    d.show();
                }
            });
            bar.addView(clr);
        }
        container.removeAllViews();
        final Forms.Seg[] sgWrap = new Forms.Seg[1];
        sgWrap[0] = new Forms.Seg(act, new String[]{"模板", "点集", "帧库"}, seg, new Runnable() {
            public void run() {
                seg = sgWrap[0].selected();
                refresh();
            }
        });
        container.addView(sgWrap[0].row);
        if (seg == 0) renderAssets(tmpls, false);
        else if (seg == 1) renderAssets(ptss, true);
        else renderFrames(frames);
    }

    // ---------------------------------------------------------------- 模板 / 点集

    private void renderAssets(List<String> list, final boolean isPts) {
        if (list.isEmpty()) {
            container.addView(Ui.emptyState(act, isPts ? "还没有点集" : "还没有模板",
                    isPts ? "点集 = 基准色 + 参考点（多点找色用）" : "模板 = 截帧框选（找图用）；先去目标画面抓一张",
                    "＋ 新建", new View.OnClickListener() {
                        public void onClick(View v) { openNew(isPts); }
                    }));
            return;
        }
        for (final String name : list) {
            LinearLayout card = Ui.card(act);
            LinearLayout head = Ui.row(act);
            final android.widget.ImageView th = new android.widget.ImageView(act);
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(Ui.dp(act, 72), Ui.dp(act, 72));
            tlp.rightMargin = Ui.dp(act, 10);
            th.setLayoutParams(tlp);
            th.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
            th.setBackground(Ui.strokedRound(act, Ui.CARD2, Ui.CARD_LINE, 6));
            head.addView(th);
            LinearLayout mid = Ui.col(act);
            mid.addView(Ui.monoBold(act, name, 14, Ui.TEXT));
            final TextView meta = Ui.mono(act, "…", 11, Ui.DIM);
            mid.addView(meta);
            head.addView(mid, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            card.addView(head);
            loadRow(name, isPts, th, meta);
            LinearLayout btns = Ui.row(act);
            btns.setGravity(Gravity.END);
            TextView pv = Ui.btnFlat(act, "预览", Ui.DIM);
            pv.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { previewDialog(name, isPts); }
            });
            TextView ed = Ui.btnFlat(act, "✎ 编辑", Ui.ACCENT);
            ed.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { openEdit(name, isPts); }
            });
            TextView ts = Ui.btnFlat(act, "试一下", Ui.ACCENT);
            ts.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { openTest(name, isPts); }
            });
            TextView rn = Ui.btnFlat(act, "改名", Ui.DIM);
            rn.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { renameDialog(name, isPts); }
            });
            TextView dl = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
            dl.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { deleteDialog(name, isPts); }
            });
            btns.addView(pv);
            btns.addView(ed);
            btns.addView(ts);
            btns.addView(rn);
            btns.addView(dl);
            card.addView(btns);
            container.addView(card);
        }
    }

    private void loadRow(final String name, final boolean isPts,
                         final android.widget.ImageView th, final TextView meta) {
        Ui.bg(new Runnable() {
            public void run() {
                Bitmap bmp;
                String metaTxt;
                if (isPts) {
                    Assets.Pts p = Assets.readPts(name);
                    bmp = Assets.thumbPts(name, Ui.dp(act, 72));
                    metaTxt = p == null ? "读取失败"
                            : String.format(java.util.Locale.US, "基准 #%06X · %d 点 · 基差 %d", p.baseRgb & 0xFFFFFF, p.pts.length, p.baseTol);
                } else {
                    Assets.Tmpl t = Assets.readTmpl(name);
                    bmp = Assets.thumbTmpl(name, Ui.dp(act, 72));
                    metaTxt = t == null ? "读取失败"
                            : (t.w + "×" + t.h + (t.hasXy ? " · @(" + t.x + "," + t.y + ")" : ""));
                }
                final List<String> refs = Assets.refs(name);
                final Bitmap fb = bmp;
                final String fm = metaTxt;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (fb != null) th.setImageBitmap(fb);
                        meta.setText(fm + (refs.isEmpty() ? " · 未引用" : " · 被 " + refs.size() + " 处引用"));
                    }
                });
            }
        }, null);
    }

    private void previewDialog(final String name, final boolean isPts) {
        final Ui.Dlg d = Ui.dialog(act, (isPts ? "点集 " : "模板 ") + name);
        final android.widget.ImageView big = new android.widget.ImageView(act);
        int sz = Ui.dp(act, 220);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(sz, sz);
        big.setLayoutParams(blp);
        big.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
        big.setBackground(Ui.strokedRound(act, Ui.CARD2, Ui.CARD_LINE, 8));
        d.body.addView(big);
        final TextView info = Ui.mono(act, "…", 12, Ui.DIM);
        info.setPadding(0, Ui.dp(act, 8), 0, 0);
        d.body.addView(info);
        Ui.bg(new Runnable() {
            public void run() {
                final Bitmap bmp = isPts ? Assets.thumbPts(name, Ui.dp(act, 220)) : Assets.thumbTmpl(name, Ui.dp(act, 220));
                String t;
                if (isPts) {
                    Assets.Pts p = Assets.readPts(name);
                    t = p == null ? "读取失败" : String.format(java.util.Locale.US,
                            "基准 #%06X · %d 参考点 · 基差 %d%s", p.baseRgb & 0xFFFFFF, p.pts.length, p.baseTol,
                            p.hasXy ? (" · @(" + p.x + "," + p.y + ")") : "");
                } else {
                    Assets.Tmpl tm = Assets.readTmpl(name);
                    t = tm == null ? "读取失败" : (tm.w + "×" + tm.h + " · rot " + tm.rot
                            + (tm.hasXy ? (" · @(" + tm.x + "," + tm.y + ")") : "")
                            + (tm.avgRgb >= 0 ? String.format(java.util.Locale.US, " · 平均色 #%06X", tm.avgRgb & 0xFFFFFF) : ""));
                }
                final List<String> refs = Assets.refs(name);
                final String ft = t;
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (bmp != null) big.setImageBitmap(bmp);
                        StringBuilder sb = new StringBuilder(ft);
                        if (refs.isEmpty()) sb.append("\n引用：无");
                        else {
                            sb.append("\n引用：");
                            for (int i = 0; i < refs.size() && i < 4; i++) {
                                if (i > 0) sb.append("；");
                                sb.append(refs.get(i));
                            }
                            if (refs.size() > 4) sb.append(" 等 ").append(refs.size()).append(" 处");
                        }
                        info.setText(sb.toString());
                    }
                });
            }
        }, null);
        TextView ok = Ui.btnSmall(act, "好");
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        d.actions.addView(ok);
        d.show();
    }

    private void renameDialog(final String name, final boolean isPts) {
        final EditText et = Ui.textField(act, "新名字（1..15 字，可用中文）");
        et.setText(name);
        final Ui.Dlg d = Ui.dialog(act, "改名 " + name);
        d.body.addView(et);
        final List<String> refs = Assets.refs(name);
        if (!refs.isEmpty()) {
            TextView w = Ui.tv(act, "注意：被 " + refs.size() + " 处引用——改名后引用会失效", 11, Ui.WARN, false);
            w.setPadding(0, Ui.dp(act, 6), 0, 0);
            d.body.addView(w);
        }
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView ok = Ui.btnSmallPrimary(act, "改名");
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String nn = et.getText().toString().trim();
                if (!Assets.nameOk(nn)) {
                    Ui.toast("名称不合法（1..15 个可见字符，可用中文）");
                    return;
                }
                if (Assets.exists(nn)) {
                    Ui.toast("已存在同名素材（模板/点集共用名字空间）");
                    return;
                }
                d.dismiss();
                if (Assets.rename(name, nn, isPts)) {
                    Ui.toast("已改名 " + name + " → " + nn);
                    refresh();
                } else {
                    Ui.toast("改名失败（源文件不存在？）");
                }
            }
        });
        d.actions.addView(cancel);
        d.actions.addView(ok);
        d.show();
    }

    private void deleteDialog(final String name, final boolean isPts) {
        final Ui.Dlg d = Ui.dialog(act, "删除" + (isPts ? "点集 " : "模板 ") + name + "？");
        final List<String> refs = Assets.refs(name);
        StringBuilder sb = new StringBuilder();
        if (refs.isEmpty()) {
            sb.append("没有被任何操作引用。");
        } else {
            sb.append("被以下操作引用（删除后核心按「模板不存在」处理）：\n");
            for (int i = 0; i < refs.size() && i < 6; i++) sb.append("· ").append(refs.get(i)).append('\n');
            if (refs.size() > 6) sb.append("… 共 ").append(refs.size()).append(" 处\n");
        }
        d.body.addView(Ui.dlgMessage(act, sb.toString()));
        TextView cancel = Ui.btnFlat(act, "取消", Ui.DIM);
        cancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { d.dismiss(); }
        });
        TextView ok = Ui.btnSmallTonal(act, "仍要删除", Ui.WARN);
        ok.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                d.dismiss();
                if (Assets.delete(name)) {
                    Ui.toast("已删除 " + name);
                    refresh();
                } else {
                    Ui.toast("删除失败");
                }
            }
        });
        d.actions.addView(cancel);
        d.actions.addView(ok);
        d.show();
    }

    private void openNew(boolean isPts) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = isPts ? VisualPage.MODE_PTS_NEW : VisualPage.MODE_TMPL_NEW;
        VisualPage.open(s, new VisualPage.Done() {
            public void onTmplSaved(String name) { refresh(); }
            public void onPointPicked(int lx, int ly) { }
            public void onColorPicked(int rgb) { }
            public void onCanceled() { }
        });
    }

    private void openEdit(String name, boolean isPts) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = isPts ? VisualPage.MODE_PTS_EDIT : VisualPage.MODE_TMPL_EDIT;
        s.name = name;
        VisualPage.open(s, new VisualPage.Done() {
            public void onTmplSaved(String n) { refresh(); }
            public void onPointPicked(int lx, int ly) { }
            public void onColorPicked(int rgb) { }
            public void onCanceled() { }
        });
    }

    private void openTest(String name, boolean isPts) {
        VisualPage.Session s = new VisualPage.Session();
        s.mode = isPts ? VisualPage.MODE_PTS_TEST : VisualPage.MODE_TMPL_TEST;
        s.name = name;
        VisualPage.open(s, null);
    }

    // ---------------------------------------------------------------- 帧库

    private void renderFrames(List<FrameStore.Entry> frames) {
        if (frames.isEmpty()) {
            container.addView(Ui.emptyState(act, "帧库还没有帧",
                    "在目标画面点悬浮浮标抓帧（或这里点 [去抓帧]）",
                    "去抓帧", new View.OnClickListener() {
                        public void onClick(View v) {
                            CaptureChip.start(act, true);
                            try {
                                if (act instanceof android.app.Activity) ((android.app.Activity) act).moveTaskToBack(true);
                            } catch (Throwable ig) { }
                        }
                    }));
            return;
        }
        for (final FrameStore.Entry e : frames) {
            LinearLayout card = Ui.card(act);
            LinearLayout head = Ui.row(act);
            final android.widget.ImageView th = new android.widget.ImageView(act);
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(Ui.dp(act, 72), Ui.dp(act, 72));
            tlp.rightMargin = Ui.dp(act, 10);
            th.setLayoutParams(tlp);
            th.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
            th.setBackground(Ui.strokedRound(act, Ui.CARD2, Ui.CARD_LINE, 6));
            head.addView(th);
            loadFrameThumb(e, th);
            LinearLayout mid = Ui.col(act);
            mid.addView(Ui.mono(act, shortTime(e.ts), 13, Ui.TEXT));
            mid.addView(Ui.mono(act, e.w + "×" + e.h + " · " + (e.rot == 0 || e.rot == 2 ? "竖" : "横"), 11, Ui.DIM));
            head.addView(mid, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            TextView view = Ui.btnFlat(act, "查看", Ui.ACCENT);
            view.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    VisualPage.Session s = new VisualPage.Session();
                    s.mode = VisualPage.MODE_VIEW;
                    s.frameName = e.name;
                    VisualPage.open(s, null);
                }
            });
            TextView fdel = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
            fdel.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    final Ui.Dlg d = Ui.dialog(act, "删除帧 " + shortTime(e.ts) + "？");
                    d.body.addView(Ui.dlgMessage(act, "从帧库删除这一帧（不可恢复）。"));
                    TextView no = Ui.btnFlat(act, "取消", Ui.DIM);
                    no.setOnClickListener(new View.OnClickListener() {
                        public void onClick(View v) { d.dismiss(); }
                    });
                    TextView yes = Ui.btnSmallTonal(act, "删除", Ui.WARN);
                    yes.setOnClickListener(new View.OnClickListener() {
                        public void onClick(View v) {
                            d.dismiss();
                            FrameStore.delete(e);
                            refresh();
                        }
                    });
                    d.actions.addView(no);
                    d.actions.addView(yes);
                    d.show();
                }
            });
            head.addView(view);
            head.addView(fdel);
            card.addView(head);
            container.addView(card);
        }
    }

    private void loadFrameThumb(final FrameStore.Entry e, final android.widget.ImageView th) {
        Ui.bg(new Runnable() {
            public void run() {
                final Bitmap bmp = FrameStore.loadBitmap(e);
                if (bmp == null) return;
                // 缩略：等比缩到 72dp
                int maxPx = Ui.dp(act, 72);
                int scale = Math.max(1, Math.max((bmp.getWidth() + maxPx - 1) / maxPx, (bmp.getHeight() + maxPx - 1) / maxPx));
                final Bitmap small = Bitmap.createScaledBitmap(bmp, Math.max(1, bmp.getWidth() / scale), Math.max(1, bmp.getHeight() / scale), true);
                Ui.MAIN.post(new Runnable() {
                    public void run() { th.setImageBitmap(small); }
                });
            }
        }, null);
    }

    private static String shortTime(long ts) {
        return new java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.US).format(new java.util.Date(ts));
    }
}
