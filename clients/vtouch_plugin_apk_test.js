/* =============================================================
 * vtouch 应用插件(APK) 同步式真机测试
 *
 * 用精简同步版 SDK：plugins.load('org.vtouch.plugin') 得到 VTouch，
 * 业务放进 vt.run() 子线程，全程同步式写法。
 *
 * 运行: AutoJs6 直接运行本文件，或
 *   adb shell am start -a android.intent.action.VIEW \
 *       -n org.autojs.autojs6/org.autojs.autojs.external.open.RunIntentActivity \
 *       --es path /sdcard/vtouch-merge/vtouch_plugin_apk_test.js
 * 结果: /sdcard/vtouch-merge/plugin_test.log
 * ============================================================= */
"use strict";

var LOG = "/sdcard/vtouch-merge/plugin_test.log";
var TOUCH_TEST = true;   // false=仅加载验证；true=连接+触摸

function note(msg) {
    try { files.append(LOG, new Date().toISOString() + " " + msg + "\n"); } catch (e) {}
    log("[vtouch-plugin] " + msg);
}
files.write(LOG, "");
note("===== 同步式插件测试开始 =====");

/* ---------- 阶段 A：加载应用插件 ---------- */
var VTouch = null;
try {
    VTouch = plugins.load('org.vtouch.plugin');
} catch (e) {
    note("plugins.load 失败: " + e);
    throw e;
}
note("plugins.load 类型: " + typeof VTouch);
note("设备: " + device.model + " Android " + device.release + " " + device.width + "x" + device.height);

/* ---------- 阶段 B：连接 + 触摸（同步式） ---------- */
if (TOUCH_TEST) {
    var vt = new VTouch();
    vt.onError = function (msg) { note("服务端错误: " + msg); };
    var t0 = Date.now();

    vt.run(function () {
        vt.connect();
        note("connect 返回（耗时 " + (Date.now() - t0) + "ms）");

        /* 同步式：点击 + 滑动 */
        var f = vt.finger();
        f.tap(Math.round(vt.width / 2), Math.round(vt.height / 2), 60);
        sleep(200);
        f.swipe(200, 2000, 900, 2000, 600);
        note("触摸完成（总耗时 " + (Date.now() - t0) + "ms）");

        vt.stop();
        note("服务已停止，脚本自然结束");
    });
} else {
    note("TOUCH_TEST=false，仅加载验证通过");
}
note("===== 结束 =====");