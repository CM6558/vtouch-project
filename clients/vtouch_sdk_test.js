/*
 * vtouch SDK 自测（AutoJs6 真机运行）。
 * 用法：adb push 到 /sdcard 后用 intent 拉起，或在 AutoJs6 里直接运行。
 *   adb shell am start -a android.intent.action.VIEW \
 *     -d file:///sdcard/vtouch_sdk_test.js -t application/x-javascript \
 *     -n org.autojs.autojs6/org.autojs.autojs.external.open.RunIntentActivity
 * 结果：控制台 + /sdcard/vtouch-sdk-test-result.txt；以 THROW 失败即 FAIL 行。
 * 注意：tap/swipe 会在屏幕上真实发生。
 */
"use strict";

var RESULT = "/sdcard/vtouch-sdk-test-result.txt";
var results = [];

function check(name, fn) {
    try {
        fn();
        results.push("PASS " + name);
    } catch (e) {
        results.push("FAIL " + name + " :: " + e);
    }
}

function report() {
    var s = results.join("\n");
    log(s);
    try { files.write(RESULT, s); } catch (e) { log("写结果失败: " + e); }
}

/* 阶段 1：加载被测 SDK。优先直测 /sdcard/vtouch.js（当前构建产物），
 * 再依次尝试项目插件与应用插件通道（记录各通道 VERSION，用于发现机上旧包）。 */
var VTouch = null;
var loadVia = "";
var tried = [];
try {
    try {
        VTouch = require("/sdcard/vtouch.js");
        loadVia = "require:/sdcard/vtouch.js";
    } catch (e0) {
        tried.push("require:" + e0);
        try {
            VTouch = plugins.load("vtouch");
            loadVia = "project-plugin";
        } catch (e2) {
            tried.push("project-plugin:" + e2);
            VTouch = plugins.load("org.vtouch.plugin");
            loadVia = "plugin-apk";
        }
    }
    results.push("INFO load via=" + loadVia + " VERSION=" + (VTouch.VERSION || "?"));
    if (tried.length) results.push("INFO fallbacks tried: " + tried.join(" | "));
    if (!VTouch.VERSION || VTouch.VERSION.indexOf("plugin") < 0) {
        results.push("WARN unexpected SDK build (not current vtouch.js)");
    }
} catch (e) {
    results.push("FAIL load :: " + e);
    results.push("RESULT=FAILED");
    report();
    exit();
}

/* 阶段 2：连接 + 触摸（阻塞调用放业务线程，主线程留给事件循环） */
var vt = null;
try {
    vt = new VTouch();
    results.push("INFO construct ok");
} catch (e) {
    results.push("FAIL construct :: " + e);
    results.push("RESULT=FAILED");
    report();
    exit();
}
var t0 = Date.now();

threads.start(function () {
    try {
        check("ready(20s)", function () { vt.ready(20000); });
        results.push("INFO connect_ms=" + (Date.now() - t0));

        var cx = Math.floor(device.width / 2);
        var cy = Math.floor(device.height / 2);

        check("finger.tap", function () {
            vt.finger().tap(cx, cy, 60);
            sleep(300);
        });
        check("finger.swipe", function () {
            vt.finger().swipe(300, 2000, 900, 2000, 400);
            sleep(300);
        });
        check("frame.2finger", function () {
            var a = vt.finger(0);
            var b = vt.finger(1);
            vt.frame([
                { slot: a.slot, state: "down", x: cx - 100, y: cy },
                { slot: b.slot, state: "down", x: cx + 100, y: cy }
            ]);
            sleep(200);
            if (a.state() !== "down" || b.state() !== "down") throw new Error("down state=" + a.state() + "/" + b.state());
            vt.frame([
                { slot: a.slot, state: "up", x: cx - 100, y: cy },
                { slot: b.slot, state: "up", x: cx + 100, y: cy }
            ]);
            sleep(200);
            if (a.state() !== "up" || b.state() !== "up") throw new Error("stuck state=" + a.state() + "/" + b.state());
        });
        check("reset", function () { vt.reset(); sleep(100); });
    } catch (e) {
        results.push("FAIL harness :: " + e);
    }
    try {
        vt.close();
        results.push("PASS close");
    } catch (e) {
        results.push("FAIL close :: " + e);
    }
    var failed = results.some(function (line) { return line.indexOf("FAIL") === 0; });
    results.push(failed ? "RESULT=FAILED" : "RESULT=ALL_PASS");
    report();
    exit();
});
