/* 区域监听调用示例：只写触发逻辑，定义全在 vtouch_bundle.js 里。
 * 顺序不能换：floaty 悬浮窗建出来后 shell(true) 会失效（回码 1），
 * 所以 ensure/connect 必须在任何悬浮窗之前。
 * 可视化 canvas 在此 ROM 上首帧必崩（Invalid ID:63），已剥离，触发用 toast+log 反馈。
 */
"use strict";
var vt = require("/sdcard/vtouch_bundle.js");

vt.ensure();
vt.connect();

var regions = vt.loadRegions();
var eng = vt.createEngine(regions, {
    onDown: function (region, f) {
        toast("按下 " + (region.name || region.id));
        log("trig down " + region.id + " s" + f.slot + " " + Math.round(f.x) + "," + Math.round(f.y));
    },
    onMove: function (region, f) {},
    onEnter: function (region, f) { log("trig enter " + region.id + " s" + f.slot); },
    onExit: function (region, f) {},
    onUp: function (region, f) {
        log("trig up " + region.id + " s" + f.slot);
        if (region.id === "btn") vt.finger().tap((region.x1 + region.x2) / 2, (region.y1 + region.y2) / 2);
    }
});

var Q = new java.util.concurrent.LinkedBlockingQueue();
function norm(p) {
    var a = p.action;
    if (a === 0 || a === "down") a = "down";
    else if (a === 1 || a === "up") a = "up";
    else a = "move";
    return { slot: p.pointerId || p.slot || 0, x: p.x, y: p.y, action: a };
}

events.observeTouch();
events.onTouch(function (p) {
    try { Q.offer(norm(p)); } catch (e) {}
});
events.on("exit", function () {
    try { vt.stop(); } catch (e) {}
});

threads.start(function () {
    log("vt-watch: on");
    while (true) {
        var p = Q.poll(200, java.util.concurrent.TimeUnit.MILLISECONDS);
        if (p) eng.feed(p);
    }
});

/* 常驻保活：main 线程靠 timer 活着（Looper 继续泵 onTouch），顺带热更新区域 */
setInterval(function () {
    try {
        regions = vt.loadRegions();
        eng.setRegions(regions);
    } catch (e) {}
}, 2000);
