package com.vtouch.editor;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

/**
 * 测试探针服务基类：可见小叠加层（56dp 胶囊）+ 无头测试台。
 *
 * 为什么必须有可见窗口（实测结论，2026-10-07）：
 * ColorOS 的 OplusHansManager 会在应用退后台后约 13 秒强制"速冻"进程——前台服务拦不住，
 * 冻结期间所有线程停摆、测试半途卡死；但**可见悬浮窗把应用 importance 抬为 floatWindow**，
 * 日志明确 `cannot transition from R to M, importance=floatWindow`：胶囊在，速冻进不来。
 * 因此测试期间保持胶囊可见是"无感测试"的必要条件（胶囊 = 唯一允许出现的界面，跑完自动消失）。
 *
 * 触发：su -c "am start-foreground-service -n com.vtouch.editor/.ChipService"
 *      su -c "am start-foreground-service -n com.vtouch.editor/.RenderService"（等价兼容入口）
 * 交互：出现即跑一轮；点按 = 再跑；长按 = 立即收起；结果展示 8 秒后自动收起。
 * 通知：MIN 级渠道（无声、无状态栏图标）。
 */
public class ProbeService extends Service {
    private final Handler h = new Handler(Looper.getMainLooper());
    private WindowManager wm;
    private TextView chip;
    private boolean added = false;
    private boolean busy = false;
    private int pollTicks = 0;

    private final Runnable hideAll = new Runnable() {
        public void run() {
            h.removeCallbacks(checkIdle);
            removeChip();
            try { stopForeground(true); } catch (Throwable ig) { }
            stopSelf();
        }
    };

    /** 已有别家在跑时：保持胶囊（防冻），每 2 秒探一次，最多 40 秒。 */
    private final Runnable checkIdle = new Runnable() {
        public void run() {
            if (busy) return;
            if (TestBench.isRunning()) {
                if (pollTicks++ < 20) {
                    setText("忙", Ui.DIM);
                    h.postDelayed(this, 2000);
                } else {
                    hideAll.run();
                }
            } else {
                startRun(null);
            }
        }
    };

    private final Runnable hardStop = new Runnable() {
        public void run() {
            if (busy) {
                TestBench.appendLog(ProbeService.this, "test_run.txt", "[FAIL] 探针看护：3 分钟未完成，收起");
                hideAll.run();
            }
        }
    };

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        startAsForeground(this, "vtouch 测试");
        if (!added) addChip();
        h.removeCallbacks(hideAll);
        h.removeCallbacks(checkIdle);
        pollTicks = 0;
        final String mode = intent != null ? intent.getStringExtra("mode") : null;
        if (!busy) {
            if (TestBench.isRunning()) h.post(checkIdle);
            else startRun(mode);
        }
        return START_NOT_STICKY;
    }

    private void addChip() {
        try {
            wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            chip = new TextView(this);
            chip.setText("测");
            chip.setTextSize(12);
            chip.setTextColor(0xFFFFFFFF);
            chip.setGravity(Gravity.CENTER);
            chip.setClickable(true);
            chip.setBackground(Ui.pressable(this, 0xF21B2230, 0x40FFFFFF, 24));
            chip.setMinWidth(Ui.dp(this, 48));
            chip.setMinHeight(Ui.dp(this, 48));
            chip.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    h.removeCallbacks(hideAll);
                    if (!busy) {
                        pollTicks = 0;
                        if (TestBench.isRunning()) h.post(checkIdle);
                        else startRun(null);
                    }
                }
            });
            chip.setOnLongClickListener(new View.OnLongClickListener() {
                public boolean onLongClick(View v) {
                    h.post(hideAll);
                    return true;
                }
            });
            int type = Build.VERSION.SDK_INT >= 26
                    ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                    : WindowManager.LayoutParams.TYPE_PHONE;
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    type,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                    PixelFormat.TRANSLUCENT);
            lp.gravity = Gravity.TOP | Gravity.RIGHT;
            lp.x = Ui.dp(this, 12);
            lp.y = (int) (getResources().getDisplayMetrics().heightPixels * 0.38f);
            wm.addView(chip, lp);
            added = true;
        } catch (Throwable t) {
            TestBench.appendLog(this, "chip_error.txt", "悬浮窗添加失败（权限缺失？）: " + t);
        }
    }

    private void startRun(final String mode) {
        busy = true;
        setText("…", Ui.ACCENT);
        final ContextThemeWrapper ctx = new ContextThemeWrapper(
                getApplicationContext(), android.R.style.Theme_Material_NoActionBar);
        Su.init(getCacheDir().getAbsolutePath());
        new Thread(new Runnable() {
            public void run() {
                String summary;
                try {
                    summary = TestBench.run(ctx, mode, new TestBench.Progress() {
                        public void step(final String label, final int idx, final int total) {
                            h.post(new Runnable() {
                                public void run() { setText(idx + "/" + total, Ui.ACCENT); }
                            });
                        }
                    });
                } catch (Throwable t) {
                    summary = "异常：" + t;
                }
                final String s = summary;
                h.post(new Runnable() {
                    public void run() {
                        busy = false;
                        boolean ok = s.contains("失败 0") && !s.contains("异常") && !s.contains("busy");
                        setText(ok ? "✓" : "✗", ok ? Ui.OK : Ui.WARN);
                        h.postDelayed(hideAll, 8000);
                    }
                });
            }
        }, "probe-run").start();
        h.postDelayed(hardStop, 180000L);
    }

    private void setText(String t, int color) {
        if (chip != null) {
            chip.setText(t);
            chip.setTextColor(color);
        }
    }

    private void removeChip() {
        if (added && wm != null && chip != null) {
            try { wm.removeView(chip); } catch (Throwable ig) { }
        }
        added = false;
    }

    @Override public void onDestroy() {
        removeChip();
        h.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    /** 前台服务化（MIN 级通知渠道，无声、无状态栏图标）。失败写 svc_error.txt，不抛出。 */
    static void startAsForeground(Service s, String title) {
        try {
            String ch = "vtouch-render";
            if (Build.VERSION.SDK_INT >= 26) {
                NotificationChannel nc = new NotificationChannel(ch, "测试", NotificationManager.IMPORTANCE_MIN);
                ((NotificationManager) s.getSystemService(Context.NOTIFICATION_SERVICE)).createNotificationChannel(nc);
            }
            Notification n = Build.VERSION.SDK_INT >= 26
                    ? new Notification.Builder(s, ch)
                        .setSmallIcon(android.R.drawable.ic_menu_view).setContentTitle(title).build()
                    : new Notification.Builder(s)
                        .setSmallIcon(android.R.drawable.ic_menu_view).setContentTitle(title).build();
            s.startForeground(1, n);
        } catch (Throwable t) {
            TestBench.appendLog(s, "svc_error.txt", "startForeground 失败: " + t);
        }
    }
}
