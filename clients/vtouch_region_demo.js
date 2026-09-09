/* 区域监听调用示例：只写触发逻辑，定义全在 vtouch_bundle.js 里。
 * UI/overlay 源码也在 SDK 里，eval 一行进主上下文即可用：
 *   eval(vt.uiSource); ui();          // 打开区域管理（再调一次关闭）
 *   ovPreview(true/false);            // 预览显隐
 * 顺序不能换：ensure/connect 必须在 ovShow 之前（悬浮窗建出来后 shell(true) 回码 1）；
 * 退出时先 stop 再关窗。
 */
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);

vt.ensure();
vt.connect();

var regions = vt.loadRegions();
ovShow(regions);

var eng = vt.createEngine(regions, {
    onDown: function (region, f) {
        ovFlash(region.id);
        log("trig down " + region.id + " s" + f.slot + " " + Math.round(f.x) + "," + Math.round(f.y));
    },
    onMove: function (region, f) {},
    onEnter: function (region, f) { ovFlash(region.id); },
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
    try { Q.offer(norm(p)); ovUpdate(eng.fingers()); } catch (e) {}
});
events.on("exit", function () {
    try { vt.stop(); } catch (e) {}
    try { ovClose(); } catch (e2) {}
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
        ovSet(regions);
    } catch (e) {}
}, 2000);

// ui();  // 需要管理区域时取消注释：列表查看+开关+删除，＋矩形/圆形框选，预览开/关
