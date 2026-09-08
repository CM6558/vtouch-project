/*
 * vtouch 应用插件（APK）使用样例：验证 plugins.load('org.vtouch.plugin') 可用。
 *
 * 分阶段设计（安全）：
 *   A. 加载应用插件并断言导出（只读，不启动任何服务）
 *   B. 预检服务二进制 /data/local/tmp/vtouchmerge|vtouchws（只读）
 *   C. 连接 + 触摸测试（会启动 vtouchmerge，EVIOCGRAB 独占物理触摸）——
 *      确认 A/B 通过后，把 TOUCH_TEST 改为 true 再运行。
 *
 * 运行方式（AutoJs6 中直接运行本文件，或 adb 触发见注释底部）。
 * 结果写日志 /sdcard/vtouch-merge/plugin_test.log，可用 adb cat 读回验证。
 */
"use strict";

var LOG = "/sdcard/vtouch-merge/plugin_test.log";
var TOUCH_TEST = true;    // 阶段 C 开关（已确认 A/B 通过）

function note(msg) {
    try { files.append(LOG, new Date().toISOString() + " " + msg + "\n"); } catch (e) {}
    console.log("[vtouch-plugin] " + msg);
}
files.write(LOG, "");
note("===== 插件测试开始 =====");

/* ---------- 阶段 A：加载应用插件 ---------- */
var VTouch = null;
try {
    VTouch = plugins.load('org.vtouch.plugin');
} catch (e) {
    note("plugins.load 失败: " + e);
    throw e;
}
note("plugins.load 返回类型: " + typeof VTouch);
note("VTouch.VERSION: " + (VTouch && VTouch.VERSION || "(无)"));
note("VTouch.Finger: " + typeof (VTouch && VTouch.Finger));
note("设备: " + device.model + " Android " + device.release + " " + device.width + "x" + device.height);
note("AutoJs6: " + autojs.versionName);

/* ---------- 阶段 B：服务二进制预检（只读） ---------- */
var r = shell("ls -l /data/local/tmp/vtouchmerge /data/local/tmp/vtouchws 2>&1; echo '---'; ls -la /data/local/tmp/vtouch-runtime/ 2>&1", true);
note("预检 code=" + r.code + "\n" + (r.result || "") + (r.error || ""));

/* ---------- 阶段 B2：插件 Java API 预检（v2） ---------- */
try {
    /* 胶水层 v2 在插件模式下会覆盖 startService 走插件 Java API（__origStart 标记） */
    var checkViaProto = VTouch.prototype.startService.toString().indexOf("__origStart") >= 0;
    note("startService 已由插件 Java API 覆盖: " + checkViaProto);
} catch (e) {
    note("Java API 预检异常: " + e);
}

/* ---------- 阶段 C：连接 + 触摸测试（默认关闭） ---------- */
if (TOUCH_TEST) {
    note("开始连接 + 触摸测试（将启动 vtouchmerge，独占物理触摸）");
    var vt = new VTouch().connect(function (client) {
        note("WebSocket 已连接，开始触摸测试");
        threads.start(function () {
            client.finger().tap(Math.round(client.width / 2), Math.round(client.height / 2), 60);
            sleep(500);
            client.finger().swipe(200, 2000, 900, 2000, 600);
            note("触摸测试完成");
            toast("vtouch 插件触摸测试完成");
            /* 服务生命周期与脚本绑定：业务完成即关闭连接并停止服务，
               避免单连接 vtouchws 被旧脚本占用导致下次连接失败。 */
            client.close();
            vt.stopService();
            note("服务已停止（close + stopService），脚本将自然结束");
        });
    });
    vt.onError = function (msg) { note("服务端错误: " + msg); };
} else {
    note("TOUCH_TEST=false，跳过连接与触摸（加载验证通过）");
    toast("vtouch 插件加载成功: " + (VTouch.VERSION || "v?"));
}
note("===== 插件测试结束 =====");

/*
 * adb 触发运行（无需在 AutoJs6 里点）：
 *   adb shell am start -a android.intent.action.VIEW \
 *       -d file:///sdcard/vtouch-merge/vtouch_plugin_apk_test.js \
 *       -t application/x-javascript org.autojs.autojs6
 * 结果读回：adb shell cat /sdcard/vtouch-merge/plugin_test.log
 */
