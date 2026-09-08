package org.vtouch.plugin;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Toast;

/**
 * 最小启动 Activity。
 *
 * 用途：ColorOS(Oplus) 应用启动管理要求应用至少被用户打开过一次，才允许
 * 被其他应用后台拉起（否则 system_server 日志出现
 * "OplusAppStartupManager: prevent start ... Type ss"）。
 * 插件本身是"无界面服务宿主"，没有可打开的东西——本 Activity 提供
 * 一个一次性入口：打开即提示并立即结束，不保留界面、不进最近任务。
 */
public class MainActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            Toast.makeText(this, "VTouch 插件已激活（打开一次后即可被脚本拉起服务）",
                    Toast.LENGTH_SHORT).show();
        } catch (Throwable ignored) {
        }
        finish();
    }
}
