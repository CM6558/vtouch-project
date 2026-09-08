package org.vtouch.plugin;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * VTouch 后台托管服务。
 *
 * 职责：以 root 拉起/守护 vtouchmerge + vtouchws（新架构），供
 *   - AutoJs6 脚本显式 startService(new Intent(...).setClassName("org.vtouch.plugin", "...VTouchService"))
 *   - 其他应用/广播触发
 * 场景。SDK 的正常路径（插件 Java API / shell 直启）不依赖本 Service。
 */
public class VTouchService extends Service {

    private static final String TAG = "VTouchService";

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(TAG, "onStartCommand: " + (intent == null ? "null" : intent.getAction()));
        if (intent != null && "org.vtouch.plugin.action.STOP".equals(intent.getAction())) {
            VTouchPlugin.stopBackend();
            stopSelf();
            return START_NOT_STICKY;
        }
        boolean ok = VTouchPlugin.startBackend();
        Log.i(TAG, "backend started: " + ok);
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null; // 纯托管，无需绑定
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "onDestroy");
        super.onDestroy();
    }
}
