/* vtouch_region_example.js — 区域监听 × 虚拟触摸结合版。
 * 线程分工（AutoJs6 单线程事件，切勿打乱）：
 *   主线程：只做 observeTouch → 队列投递 + overlay 画点，立即返回，不 sleep 不发包。
 *   业务线程：poll 队列 → engine.feed（纯函数）→ 回调里调 vt 虚拟手指。
 * 物理手指只被观察不被拦截；虚拟手指只在回调里代点/代滑。
 */
"use strict";
var vt = require("/sdcard/vtouch_bundle.js");
var store = require("./regions_store.js");
var engineLib = require("./region_engine.js");
var overlay = require("./region_overlay.js");

var regions = store.loadRegions();
overlay.show(regions);

// 业务回调：只写触发后干什么。这里演示：物理手指在区域内抬起 → 虚拟手指代点中心。
var handlers = {
    onDown: function (region, f) { overlay.flash(region.id); },
    onMove: function (region, f) {},
    onEnter: function (region, f) { overlay.flash(region.id); },
    onExit: function (region, f) {},
    onUp: function (region, f) {
        if (region.id === "btn") {
            var cx = (region.x1 + region.x2) / 2, cy = (region.y1 + region.y2) / 2;
            threads.start(function () { vt.finger().tap(cx, cy); });
        }
    }
};
var eng = engineLib.createEngine(regions, handlers);

// 主→业务的点队列（线程安全），onTouch 里只投递。
var Q = new java.util.concurrent.LinkedBlockingQueue();
function norm(p) {
    // 兼容不同 ROM 的字段：x/y/action(0 down/1 up/2 move) 或字符串
    var a = p.action;
    if (a === 0 || a === "down") a = "down";
    else if (a === 1 || a === "up") a = "up";
    else a = "move";
    return { slot: p.pointerId || p.slot || 0, x: p.x, y: p.y, action: a };
}

vt.ensure();
vt.connect(); // 成功即设为当前连接，finger/frame 直接用

events.observeTouch();
events.onTouch(function (p) {
    try {
        Q.offer(norm(p));
        overlay.update(eng.fingers());
    } catch (e) {}
});

events.on("exit", function () {
    try { overlay.close(); } catch (e) {}
    try { vt.stop(); } catch (e2) {}
});

// 业务线程：唯一做 engine.feed + vt 触摸的地方。主线程保持干净泵事件。
threads.start(function () {
    device.keepScreenOn(10 * 60 * 1000);
    var lastReload = Date.now();
    while (true) {
        var p = Q.poll(200, java.util.concurrent.TimeUnit.MILLISECONDS);
        if (Date.now() - lastReload > 2000) { // UI 改库后 2s 内热更新
            regions = store.loadRegions();
            eng.setRegions(regions);
            overlay.setRegions(regions);
            lastReload = Date.now();
        }
        if (p) eng.feed(p);
    }
});
