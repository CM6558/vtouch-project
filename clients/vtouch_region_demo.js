/* 区域监听调用示例：只写触发逻辑，定义全在 vtouch_bundle.js 里。
 * 主线程只投递触摸点，业务线程 feed + 虚拟手指。结束时自动清叠加层 + 停二进制。
 */
"use strict";
var vt = require("/sdcard/vtouch_bundle.js");

var regions = vt.loadRegions();
vt.ovShow(regions);

var eng = vt.createEngine(regions, {
    onDown: function (region, f) { vt.ovFlash(region.id); },
    onMove: function (region, f) {},
    onEnter: function (region, f) { vt.ovFlash(region.id); },
    onExit: function (region, f) {},
    onUp: function (region, f) {
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

vt.ensure();
vt.connect();

events.observeTouch();
events.onTouch(function (p) {
    try { Q.offer(norm(p)); vt.ovUpdate(eng.fingers()); } catch (e) {}
});
events.on("exit", function () {
    try { vt.ovClose(); } catch (e) {}
    try { vt.stop(); } catch (e2) {}
});

threads.start(function () {
    device.keepScreenOn(10 * 60 * 1000);
    var last = Date.now();
    while (true) {
        var p = Q.poll(200, java.util.concurrent.TimeUnit.MILLISECONDS);
        if (Date.now() - last > 2000) {
            regions = vt.loadRegions();
            eng.setRegions(regions);
            vt.ovSetRegions(regions);
            last = Date.now();
        }
        if (p) eng.feed(p);
    }
});
