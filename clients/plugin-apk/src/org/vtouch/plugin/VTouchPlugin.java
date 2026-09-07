package org.vtouch.plugin;

import android.content.ComponentName;
import android.content.Context;
import android.content.ServiceConnection;
import android.os.IBinder;

/**
 * VTouch AutoJs6 应用插件注册类。
 *
 * AutoJs6 (6.7.0+) 通过 meta-data "org.autojs.plugin.sdk.registry" 找到本类，
 * 反射调用静态方法 loadDefault(Context, Context, Object, Object) 得到插件实例。
 * 要求 (见 AutoJs6 源码 Plugin.kt):
 *   1. 实例必须是 android.content.ServiceConnection 类型（硬性检查，否则抛
 *      "Plugin instance must be type of android.content.ServiceConnection"）。
 *   2. 反射调 getVersion()/getAssetsScriptDir() 等方法（鸭子类型即可）。
 *   3. getVersion() < 2 时跳过 Service 绑定（bindService 仅在 version >= 2 触发），
 *      纯脚本插件返回 1 即可，无需真正实现 Service/AIDL。
 *
 * 另需在 AutoJs6 "插件中心" 手动授权一次（签名指纹不在官方/信任列表）。
 */
public class VTouchPlugin implements ServiceConnection {

    public static Object loadDefault(Context context, Context selfContext, Object runtime, Object topLevelScope) {
        return new VTouchPlugin();
    }

    /** 插件 JS 胶水层所在的 assets 目录，其下必须存在 index.js。 */
    public String getAssetsScriptDir() {
        return "vtouch";
    }

    /** 经典纯脚本插件版本号：<2 跳过 Service 绑定。 */
    public int getVersion() {
        return 1;
    }

    /* ServiceConnection 接口必需方法（纯脚本模式下不会被调用）。 */
    @Override
    public void onServiceConnected(ComponentName name, IBinder service) {
    }

    @Override
    public void onServiceDisconnected(ComponentName name) {
    }
}
