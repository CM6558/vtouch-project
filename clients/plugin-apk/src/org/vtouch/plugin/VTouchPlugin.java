package org.vtouch.plugin;

import android.content.Context;

/**
 * VTouch AutoJs6 应用插件注册类。
 *
 * AutoJs6 通过 meta-data "org.autojs.plugin.sdk.registry" 找到本类，
 * 再反射调用静态方法 loadDefault(Context, Context, Object, Object)
 * (见 AutoJs6 源码 Plugin.java: Class.forName(...).getMethod("loadDefault", ...))。
 * 返回对象的 getAssetsScriptDir()/getVersion() 同样由反射调用，因此
 * 无需继承任何 SDK 基类 —— 鸭子类型即可，零依赖。
 */
public class VTouchPlugin {

    public static Object loadDefault(Context context, Context selfContext, Object runtime, Object topLevelScope) {
        return new VTouchPlugin();
    }

    /** 插件 JS 胶水层所在的 assets 目录，其下必须存在 index.js。 */
    public String getAssetsScriptDir() {
        return "vtouch";
    }

    /** 插件版本号。 */
    public String getVersion() {
        return "1.0.0";
    }
}
