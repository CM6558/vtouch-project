package org.vtouch.plugin;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * VTouch 后台服务宿主（v2 主路径）。
 *
 * 职责：作为 AutoJs6 应用插件的"服务运行"宿主，负责以 root 拉起/停止
 * vtouchmerge + vtouchws 后端，并用前台服务保护自身（避免脚本运行中
 * 被系统回收、daemon 失主）。
 *
 * 生命周期（与脚本绑定）：
 *   - 脚本 load 插件 -> VTouchPlugin.startVTouchService() -> startService(本服务)
 *     -> root 启动后端 -> isServiceReady() 轮询确认
 *   - 脚本退出 -> VTouchPlugin.stopVTouchService() -> startService(STOP action)
 *     -> stopBackend() + stopForeground + stopSelf
 *   - onDestroy 兜底：stopBackend()（脚本强杀/服务被杀时清理 daemon，释放 EVIOCGRAB）
 *
 * 构建约束：本机仅 android-24 platform，因此
 *   - NotificationChannel(API 26+) 用反射创建（不直接引用新 API 类）
 *   - 2 参 startForeground(id, notification)（API 5+，android-24 可编译）
 *   - targetSdk=28 声明在 Manifest，规避 Android 14+ 的
 *     MissingForegroundServiceTypeException（无需 specialUse 类型与权限）
 */
public class VTouchService extends Service {

    private static final String TAG = "VTouchService";
    private static final String CHANNEL_ID = "vtouch";
    private static final int NOTIF_ID = 1;
    public static final String ACTION_STOP = "org.vtouch.plugin.action.STOP";

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();
        startForeground(NOTIF_ID, buildNotification());
        Log.i(TAG, "onCreate: foreground started");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        Log.i(TAG, "onStartCommand: " + action);
        if (ACTION_STOP.equals(action)) {
            VTouchPlugin.stopBackend();
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        boolean ok = VTouchPlugin.startBackend();
        Log.i(TAG, "backend started: " + ok);
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "onDestroy");
        VTouchPlugin.stopBackend();
        stopForeground(true);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null; // 纯托管，无需绑定
    }

    /** API 26+ 用反射创建通知渠道（android-24 编译期无此类，不能直接引用）。 */
    private void ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return;
        try {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            Class<?> cls = Class.forName("android.app.NotificationChannel");
            Object ch = cls.getConstructor(String.class, CharSequence.class, int.class)
                    .newInstance(CHANNEL_ID, "vtouch", 2 /* IMPORTANCE_LOW */);
            nm.getClass().getMethod("createNotificationChannel", cls).invoke(nm, ch);
            Log.i(TAG, "notification channel ensured");
        } catch (Throwable t) {
            Log.w(TAG, "ensureChannel failed: " + t);
        }
    }

    private Notification buildNotification() {
        Notification.Builder b = new Notification.Builder(this)
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentTitle("VTouch")
                .setContentText("vtouch 服务运行中")
                .setOngoing(true)
                .setPriority(Notification.PRIORITY_LOW);
        if (Build.VERSION.SDK_INT >= 26) {
            try {
                b.getClass().getMethod("setChannelId", String.class).invoke(b, CHANNEL_ID);
            } catch (Throwable t) {
                Log.w(TAG, "setChannelId failed: " + t); // 无渠道仍可跑前台服务，仅通知不显示
            }
        }
        return b.build();
    }
}
