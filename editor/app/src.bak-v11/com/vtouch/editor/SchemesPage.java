package com.vtouch.editor;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** 方案页：列表 / 切换 / 从当前另存为 / 新建空 / 重命名 / 删除（行内操作按钮）。 */
public class SchemesPage {
    private final Context act;
    private LinearLayout listBox;
    private TextView statusLine;
    private String current = "";
    private List<String> names = new ArrayList<String>();
    private java.util.Map<String, String> counts = new java.util.HashMap<String, String>();

    public SchemesPage(Context a) { act = a; }

    private interface NameCb { void onName(String name); }

    public View build() {
        LinearLayout root = Ui.col(act);

        LinearLayout bar = Ui.row(act);
        bar.setPadding(Ui.dp(act, 16), Ui.dp(act, 10), Ui.dp(act, 8), Ui.dp(act, 6));
        statusLine = Ui.mono(act, "", 12, Ui.DIM);
        statusLine.setSingleLine(true);
        statusLine.setEllipsize(android.text.TextUtils.TruncateAt.END);
        bar.addView(statusLine, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView newB = Ui.btnSmall(act, "＋ 新建空方案");
        newB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { newEmptyFlow(); }
        });
        TextView saveAsB = Ui.btnSmall(act, "从当前另存为");
        saveAsB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { saveAsFlow(); }
        });
        bar.addView(newB);
        bar.addView(saveAsB);
        root.addView(bar);

        ScrollView sc = new ScrollView(act);
        listBox = Ui.col(act);
        listBox.setPadding(Ui.dp(act, 8), Ui.dp(act, 4), Ui.dp(act, 8), Ui.dp(act, 16));
        sc.addView(listBox);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        return root;
    }

    public void refresh() {
        Ui.bg(new Runnable() {
            public void run() {
                final List<String> list = Schemes.list();
                final String cur = Schemes.current();
                final java.util.Map<String, String> cnt = Schemes.counts();
                Ui.MAIN.post(new Runnable() {
                    public void run() { applyLoad(list, cur, cnt); }
                });
            }
        }, null);
    }

    private void applyLoad(List<String> list, String cur, java.util.Map<String, String> cnt) {
        names = list;
        current = cur;
        counts = cnt;
        listBox.removeAllViews();
        statusLine.setText("共 " + list.size() + " 个方案");
        if (list.isEmpty()) {
            listBox.addView(Ui.emptyState(act, "还没有方案",
                    "方案保存了一套区域 + 操作的组合，可随时切换。",
                    "＋ 新建空方案", new View.OnClickListener() {
                        public void onClick(View v) { newEmptyFlow(); }
                    }));
            return;
        }
        for (final String name : list) {
            final boolean isCur = name.equals(cur);
            LinearLayout card = Ui.card(act);
            if (isCur) card.setBackground(Ui.cardGlow(act));
            LinearLayout titleRow = Ui.row(act);
            titleRow.addView(Ui.monoBold(act, name, 15, isCur ? Ui.ACCENT : Ui.TEXT),
                    new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            if (isCur) titleRow.addView(Ui.chipSolid(act, "当前", Ui.ACCENT, Ui.ON_ACCENT));
            if (!isCur) {
                TextView sw = Ui.btnFlat(act, "切换", Ui.ACCENT);
                sw.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { switchFlow(name); }
                });
                titleRow.addView(sw);
            }
            TextView rn = Ui.btnFlat(act, "重命名", Ui.DIM);
            rn.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) { renameFlow(name); }
            });
            titleRow.addView(rn);
            if (!isCur) {
                TextView del = Ui.btnFlat(act, "✕ 删除", Ui.WARN);
                del.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) { deleteFlow(name); }
                });
                titleRow.addView(del);
            }
            card.addView(titleRow);
            String meta = counts.get(name);
            if (meta != null) {
                TextView mt = Ui.mono(act, meta, 11, Ui.DIM);
                mt.setPadding(0, Ui.dp(act, 2), 0, 0);
                card.addView(mt);
            }
            listBox.addView(card);
        }
    }

    private void switchFlow(final String name) {
        if (name.equals(current)) { Ui.toast("已是当前方案"); return; }
        final Ui.Dlg dlg = Ui.dialog(act, "切换到方案 " + name + "？");
        dlg.body.addView(Ui.dlgMessage(act, "会用该方案的 regions.conf / ops.conf 覆盖当前 live（各页未保存的编辑会丢失）。"));
        TextView cancelB = Ui.btnFlat(act, "取消", Ui.DIM);
        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { dlg.dismiss(); }
        });
        TextView okB = Ui.btnSmallPrimary(act, "切换");
        okB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                dlg.dismiss();
                Ui.bg(new Runnable() {
                    public void run() {
                        final Su.R r = Schemes.switchTo(name);
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                if (!r.ok()) { Ui.toast("切换失败：" + r.trimmed()); return; }
                                Ui.refreshHeader();
                                refresh();
                                Ui.offerRestart("已切换到方案 " + name);
                            }
                        });
                    }
                }, null);
            }
        });
        dlg.actions.addView(cancelB);
        dlg.actions.addView(okB);
        dlg.show();
    }

    private void saveAsFlow() {
        nameInputDialog("从当前另存为", new NameCb() {
            public void onName(String name) {
                Ui.bg(new Runnable() {
                    public void run() {
                        final Su.R r = Schemes.saveAs(name);
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                Ui.toast(r.ok() ? "已另存为方案 " + name : "失败：" + r.trimmed());
                                refresh();
                            }
                        });
                    }
                }, null);
            }
        });
    }

    private void newEmptyFlow() {
        nameInputDialog("新建空方案", new NameCb() {
            public void onName(String name) {
                Ui.bg(new Runnable() {
                    public void run() {
                        final Su.R r = Schemes.createEmpty(name);
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                Ui.toast(r.ok() ? "已新建空方案 " + name : "失败：" + r.trimmed());
                                refresh();
                            }
                        });
                    }
                }, null);
            }
        });
    }

    private void renameFlow(final String name) {
        nameInputDialog("重命名方案 " + name, new NameCb() {
            public void onName(String newName) {
                Ui.bg(new Runnable() {
                    public void run() {
                        final Su.R r = Schemes.rename(name, newName);
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                Ui.toast(r.ok() ? "已重命名为 " + newName : "失败：" + r.trimmed());
                                Ui.refreshHeader();
                                refresh();
                            }
                        });
                    }
                }, null);
            }
        });
    }

    private void deleteFlow(final String name) {
        if (name.equals(current)) {
            Ui.toast("当前方案不能删除（先切换到其他方案）");
            return;
        }
        final Ui.Dlg dlg = Ui.dialog(act, "删除方案 " + name + "？");
        dlg.body.addView(Ui.dlgMessage(act, "将删除整个方案目录，不可恢复。"));
        TextView cancelB = Ui.btnFlat(act, "取消", Ui.DIM);
        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { dlg.dismiss(); }
        });
        TextView okB = Ui.btnSmallTonal(act, "删除", Ui.WARN);
        okB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                dlg.dismiss();
                Ui.bg(new Runnable() {
                    public void run() {
                        final Su.R r = Schemes.delete(name);
                        Ui.MAIN.post(new Runnable() {
                            public void run() {
                                Ui.toast(r.ok() ? "已删除 " + name : "失败：" + r.trimmed());
                                refresh();
                            }
                        });
                    }
                }, null);
            }
        });
        dlg.actions.addView(cancelB);
        dlg.actions.addView(okB);
        dlg.show();
    }

    private void nameInputDialog(String title, final NameCb cb) {
        final EditText e = Ui.textField(act, "方案名（1..15 字，可用中文）");
        LinearLayout box = Ui.col(act);
        box.addView(e);
        final Ui.Dlg dlg = Ui.dialog(act, title);
        dlg.body.addView(box);
        TextView cancelB = Ui.btnFlat(act, "取消", Ui.DIM);
        cancelB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { dlg.dismiss(); }
        });
        TextView okB = Ui.btnSmallPrimary(act, "确定");
        dlg.actions.addView(cancelB);
        dlg.actions.addView(okB);
        dlg.show();
        okB.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String name = e.getText().toString().trim();
                if (!Schemes.nameOk(name)) {
                    Ui.toast("方案名非法：1..15 个可见字符（可用中文），不能是裸 \"-\"");
                    return;
                }
                if (names.contains(name)) {
                    Ui.toast("方案已存在：" + name);
                    return;
                }
                dlg.dismiss();
                cb.onName(name);
            }
        });
    }
}
