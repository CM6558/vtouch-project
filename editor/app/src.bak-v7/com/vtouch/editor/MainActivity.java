package com.vtouch.editor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;

import java.io.File;
import java.io.FileOutputStream;

/**
 * 主界面：外壳（标题 + 状态 + 页签 + 内容）由 Shell 构建，本类只管生命周期与交互回调。
 * 无头测试/渲染请用 RenderService / ChipService（am 触发，零打扰）。
 */
public class MainActivity extends Activity implements Ui.Host {
    Shell shell;

    @Override
    protected void onCreate(Bundle b) {
        // kick：从 Activity（前台）拉起测试服务后立即自我关闭——绕过 ColorOS 对后台冷启动服务的拦截。
        // 加固：try 兜底（异常也必关）+ finishAndRemoveTask（绝不留前台/留任务栈）。
        if (getIntent() != null && getIntent().getBooleanExtra("kick", false)) {
            setTheme(android.R.style.Theme_Translucent_NoTitleBar);
            super.onCreate(b);
            try {
                android.content.Intent it = getIntent();
                Class<?> c = "chip".equals(it.getStringExtra("svc")) ? ChipService.class : RenderService.class;
                android.content.Intent si = new android.content.Intent(this, c);
                if (it.hasExtra("mode")) si.putExtra("mode", it.getStringExtra("mode"));
                if (android.os.Build.VERSION.SDK_INT >= 26) startForegroundService(si);
                else startService(si);
            } catch (Throwable t) {
                TestBench.appendLog(this, "kick_error.txt", "kick 启动服务失败: " + t);
            } finally {
                finishAndRemoveTask();
            }
            return;
        }
        setTheme(android.R.style.Theme_Material_NoActionBar);
        super.onCreate(b);
        Su.init(getCacheDir().getAbsolutePath());
        Ui.HOST = this;
        shell = new Shell();
        shell.build(this);
        setContentView(shell.root);
        shell.showTab(0);
        if (getIntent() != null && getIntent().getBooleanExtra("selftest", false)) runSelfTestFlow();
        // 弹窗真机抽查：--es dlg all（12 秒自动演完并关闭）
        if (getIntent() != null && "all".equals(getIntent().getStringExtra("dlg"))) dlgShowcase();
    }

    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent != null && (intent.getBooleanExtra("selftest", false) || intent.getBooleanExtra("probe", false)))
            runSelfTestFlow();
        if (intent != null && "all".equals(intent.getStringExtra("dlg"))) dlgShowcase();
    }

    public void refreshHeader() {
        if (shell != null) shell.refreshHeader();
    }

    /** M3 Snackbar 风格：底部浮起的短暂反馈（替代 Toast）。 */
    public void toast(String s) {
        final android.view.ViewGroup root = (android.view.ViewGroup) findViewById(android.R.id.content);
        if (root == null) return;
        if (snack != null) root.removeView(snack);
        final android.widget.TextView t = new android.widget.TextView(this);
        t.setText(s);
        t.setTextSize(13);
        t.setTextColor(Ui.TEXT);
        t.setMaxWidth(Ui.dp(this, 320));
        t.setBackground(Ui.strokedRound(this, 0xF01D2126, Ui.CARD_LINE, 8));
        t.setPadding(Ui.dp(this, 16), Ui.dp(this, 12), Ui.dp(this, 16), Ui.dp(this, 12));
        t.setElevation(Ui.dp(this, 8));
        android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.WRAP_CONTENT,
                android.widget.FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = android.view.Gravity.BOTTOM | android.view.Gravity.CENTER_HORIZONTAL;
        lp.bottomMargin = Ui.dp(this, 28);
        lp.leftMargin = Ui.dp(this, 16);
        lp.rightMargin = Ui.dp(this, 16);
        root.addView(t, lp);
        t.setAlpha(0f);
        t.animate().alpha(1f).setDuration(200).start();
        snack = t;
        root.postDelayed(new Runnable() {
            public void run() {
                if (snack != t) return;
                t.animate().alpha(0f).setDuration(220).withEndAction(new Runnable() {
                    public void run() {
                        if (snack == t) {
                            android.view.ViewGroup r2 = (android.view.ViewGroup) findViewById(android.R.id.content);
                            if (r2 != null) r2.removeView(t);
                            snack = null;
                        }
                    }
                }).start();
            }
        }, 2600);
    }

    private android.view.View snack;

    /** 保存后询问是否重启核心使其生效（核心未运行则只提示）。 */
    public void offerRestart(final String what) {
        Ui.bg(new Runnable() {
            public void run() {
                final String pid = Su.corePid();
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        if (pid.isEmpty()) {
                            toast(what + "（核心未运行，无需重启）");
                            return;
                        }
                        final Ui.Dlg d = Ui.dialog(MainActivity.this, what);
                        d.body.addView(Ui.dlgMessage(MainActivity.this,
                                "核心正在运行（pid " + pid + "）。重启核心使其生效？\n（重启会短暂释放并重新抓取触摸屏）"));
                        android.widget.TextView later = Ui.btnFlat(MainActivity.this, "稍后", Ui.DIM);
                        later.setOnClickListener(new android.view.View.OnClickListener() {
                            public void onClick(android.view.View v) { d.dismiss(); }
                        });
                        android.widget.TextView go = Ui.btnSmallPrimary(MainActivity.this, "重启核心");
                        go.setOnClickListener(new android.view.View.OnClickListener() {
                            public void onClick(android.view.View v) { d.dismiss(); restartCore(); }
                        });
                        d.actions.addView(later);
                        d.actions.addView(go);
                        d.show();
                    }
                });
            }
        }, null);
    }

    void restartCore() {
        Ui.bg(new Runnable() {
            public void run() {
                Su.run("sh /sdcard/vtouch.sh stop");
                final Su.R r = Su.run("sh /sdcard/vtouch.sh start");
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        toast(r.ok() ? "核心已重启" : "重启失败：" + r.trimmed());
                        refreshHeader();
                    }
                });
            }
        }, null);
    }

    // ---------------- 弹窗真机抽查（am start --es dlg all：6s 确认弹窗 → 6s 表单弹窗 → 自动关闭） ----------------

    void dlgShowcase() {
        android.util.Log.i("vtouch-dlg", "showcase start, intent=" + getIntent());
        final Ui.Dlg[] cur = new Ui.Dlg[1];
        final android.os.Handler h = new android.os.Handler();
        cur[0] = dlgShowcaseConfirm();
        cur[0].show();
        h.postDelayed(new Runnable() {
            public void run() {
                android.util.Log.i("vtouch-dlg", "timer 6s -> form");
                cur[0].dismiss();
                cur[0] = dlgShowcaseForm();
                cur[0].show();
            }
        }, 6000);
        h.postDelayed(new Runnable() {
            public void run() {
                android.util.Log.i("vtouch-dlg", "timer 12s -> finish");
                cur[0].dismiss();
                finishAndRemoveTask();
            }
        }, 12000);
    }

    @Override
    public void onDestroy() {
        android.util.Log.i("vtouch-dlg", "MainActivity onDestroy");
        if (Ui.HOST == this) Ui.HOST = null;
        super.onDestroy();
    }

    @Override
    public void onStop() {
        android.util.Log.i("vtouch-dlg", "MainActivity onStop");
        super.onStop();
    }

    @Override
    public void onBackPressed() {
        android.util.Log.i("vtouch-dlg", "MainActivity onBackPressed");
        if (shell != null && shell.closeEditor()) return;   // 编辑页中：返回先退编辑
        super.onBackPressed();
    }

    Ui.Dlg dlgShowcaseConfirm() {
        final Ui.Dlg d = Ui.dialog(this, "删除区域 r1？");
        d.body.addView(Ui.dlgMessage(this, "引用它的操作绑定 / 门控会变为悬空（核心按未命中处理）。"));
        android.widget.TextView cB = Ui.btnFlat(this, "取消", Ui.DIM);
        cB.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { d.dismiss(); }
        });
        android.widget.TextView oB = Ui.btnSmallTonal(this, "删除", Ui.WARN);
        oB.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { d.dismiss(); }
        });
        d.actions.addView(cB);
        d.actions.addView(oB);
        return d;
    }

    Ui.Dlg dlgShowcaseForm() {
        final Ui.Dlg f = Ui.dialog(this, "编辑区域 r1");
        f.body.addView(Ui.label(this, "id（[A-Za-z0-9_-]，1..15）"));
        android.widget.EditText et = Ui.textField(this, "如 r1");
        et.setText("r1");
        f.body.addView(et);
        f.body.addView(Ui.label(this, "类型"));
        f.body.addView(Ui.dlgMessage(this, "（抽查用示例字段）"));
        android.widget.TextView cB = Ui.btnFlat(this, "取消", Ui.DIM);
        cB.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { f.dismiss(); }
        });
        android.widget.TextView oB = Ui.btnSmallPrimary(this, "保存");
        f.actions.addView(cB);
        f.actions.addView(oB);
        return f;
    }

    public void runSelfTestFlow() {
        Ui.bg(new Runnable() {
            public void run() {
                final String report = selfTest();
                try {
                    File f = new File(getExternalFilesDir(null), "selftest.txt");
                    FileOutputStream fo = new FileOutputStream(f);
                    fo.write(report.getBytes("UTF-8"));
                    fo.close();
                } catch (Exception e) { /* 忽略 */ }
                Ui.MAIN.post(new Runnable() {
                    public void run() {
                        shell.statusPage.showReport(report);
                        shell.showTab(3);
                        final Ui.Dlg d = Ui.dialog(MainActivity.this, "自检");
                        android.widget.TextView rt = Ui.mono(MainActivity.this, report, 12, Ui.TEXT);
                        rt.setTextIsSelectable(true);
                        d.body.addView(rt);
                        android.widget.TextView okB = Ui.btnSmallPrimary(MainActivity.this, "好");
                        okB.setOnClickListener(new android.view.View.OnClickListener() {
                            public void onClick(android.view.View v) { d.dismiss(); }
                        });
                        d.actions.addView(okB);
                        d.show();
                    }
                });
            }
        }, null);
    }

    /** 自检：委托测试台的数据自检（su + 两份文件往返一致性 + 方案概况）。 */
    String selfTest() {
        return TestBench.dataSelfTest();
    }
}
