/* 区域监听完整样例：5 种事件全接 + 矩形/圆形双区域。
 * 首次运行会自动往库里加一个圆形区（已有则跳过）。
 * 反馈：toast 看屏，log 看 AutoJs6 日志，绿闪看预览。
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

/* 种子圆形区：库里没有圆形才加 */
(function () {
    var rs = vt.loadRegions(), i, hasCircle = false;
    for (i = 0; i < rs.length; i++) if (rs[i].type === "circle") hasCircle = true;
    if (!hasCircle) {
        rs.push({ id: "c0", name: "圆形区", type: "circle", cx: 720, cy: 2400, r: 220, enabled: true });
        vt.rgSave(rs);
    }
})();

var regions = vt.loadRegions();
ovShow(regions);
var moveCount = 0;

var eng = vt.createEngine(regions, {
    onDown: function (region, f) {
        ovFlash(region.id);
        toast("按下 " + (region.name || region.id));
        log("ev down " + region.id + " s" + f.slot + " " + Math.round(f.x) + "," + Math.round(f.y));
    },
    onMove: function (region, f) {
        moveCount++;
        if (moveCount % 15 === 1) log("ev move " + region.id + " s" + f.slot + " " + Math.round(f.x) + "," + Math.round(f.y) + " (#" + moveCount + ")");
    },
    onUp: function (region, f) {
        log("ev up " + region.id + " s" + f.slot);
        var cx = region.type === "circle" ? region.cx : (region.x1 + region.x2) / 2;
        var cy = region.type === "circle" ? region.cy : (region.y1 + region.y2) / 2;
        toast("代点 " + (region.name || region.id));
        vt.finger().tap(cx, cy);
    },
    onEnter: function (region, f) {
        ovFlash(region.id);
        log("ev enter " + region.id + " s" + f.slot);
    },
    onExit: function (region, f) {
        log("ev exit " + region.id + " s" + f.slot);
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
