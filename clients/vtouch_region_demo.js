/* 区域监听调用示例：物理触摸流来自 vtouchd 订阅（pev），不再用 observeTouch。
 * 用法：eval(vt.uiSource) 一次；ovPreview(true/false) 预览显隐。
 * 顺序不能换：ensure/connect 必须在 ovShow 之前（悬浮窗建出来后 shell(true) 回码 1）；
 * 退出时先 stop 再关窗。
 */
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);

/* 单实例接管：后来者广播，先到者自退（daemon 单 client，双活会互踢）。 */
var MY = "" + Date.now() + "_" + Math.random();
try {
    events.broadcast.on("vt-takeover", function (tok) {
        if (tok !== MY) { try { ovClose(); } catch (e) {} try { vt.stop(); } catch (e2) {} exit(); }
    });
} catch (e) {}
try { events.broadcast.emit("vt-takeover", MY); } catch (e) {}
sleep(1500);

vt.ensure();
var c = vt.connect();

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
vt.sub(c);

events.on("exit", function () {
    try { vt.stop(); } catch (e) {}
    try { ovClose(); } catch (e2) {}
});

/* 读线程：socket -> Q（唯一消费回包的地方；业务线程发包不再 drain）。
 * 断线自愈：重做 ensure/connect/sub 后继续，不退出。 */
threads.start(function () {
    log("vt-sub: on");
    var cc = c, lastPing = 0;
    for (;;) {
        try {
            for (;;) {
                var line = cc.recv();
                if (line === null) {
                    /* available()==0 分不清空闲和断线：定期 ping，写失败即断线 */
                    if (Date.now() - lastPing > 3000) { lastPing = Date.now(); cc.send("ping"); }
                    sleep(10);
                    continue;
                }
                var e = vt.parseEv(line);
                if (e) Q.offer(e);
            }
        } catch (err) {
            log("vt-sub 重连: " + err);
            try { cc.close(); } catch (e2) {}
            sleep(1000);
            try { vt.ensure(); cc = vt.connect(); vt.sub(cc); c = cc; }
            catch (e3) { sleep(2000); }
        }
    }
});

/* 喂线程：Q -> 引擎 -> 虚拟手指（重连空窗的 tap 异常就地吃掉）。 */
threads.start(function () {
    log("vt-watch: on");
    while (true) {
        try {
            var p = Q.take();
            if (p) { eng.feed(p); ovUpdate(eng.fingers()); }
        } catch (e) { sleep(500); }
    }
});

/* 常驻保活：main 线程靠 timer 活着，顺带热更新区域 */
setInterval(function () {
    try {
        regions = vt.loadRegions();
        eng.setRegions(regions);
        ovSet(regions);
    } catch (e) {}
}, 2000);
