/* vtouch_orient_demo.js — 肉眼验证方向换算。
 * 绿圈 = 目标点（当前屏坐标实时算），红点 = 注入 tap 的实际落点。
 * 四角各打一发，红盖绿即换算正确；转屏后自动重测。点“退出”关窗停服务。
 */
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);

/* 单实例接管 + 退出清理：否则旧窗盖在上层吃掉新 tap。 */
var MY = "orient_" + Date.now() + "_" + Math.random();
try {
    events.broadcast.on("vt-takeover", function (tok) {
        if (tok !== MY) {
            try { if (gW) gW.close(); } catch (e) {}
            try { c.close(); } catch (e2) {}
            try { vt.stop(); } catch (e3) {}
            exit();
        }
    });
} catch (e4) {}
try { events.broadcast.emit("vt-takeover", MY); } catch (e5) {}
sleep(1000);
events.on("exit", function () {
    try { if (gW) gW.close(); } catch (e6) {}
    try { vt.stop(); } catch (e7) {}
});

vt.ensure();
var c = vt.connect();

var targets = [], hits = [];
var gW = null, gLoc = null;

function curTargets() {
    /* 边距 0.2：顶部标题栏会吃掉落点，角点必须让开标题和按钮。 */
    var w = device.width, h = device.height, m = 0.2;
    return [
        { x: Math.round(w * m), y: Math.round(h * m) },
        { x: Math.round(w * (1 - m)), y: Math.round(h * m) },
        { x: Math.round(w * m), y: Math.round(h * (1 - m)) },
        { x: Math.round(w * (1 - m)), y: Math.round(h * (1 - m)) }
    ];
}

function redraw() { try { gW.board.postInvalidate(); } catch (e) {} }

function runTest() {
    threads.start(function () {
        try {
            targets = curTargets(); hits = [];
            redraw();
            sleep(400);
            for (var i = 0; i < targets.length; i++) {
                vt.finger().tap(targets[i].x, targets[i].y);
                sleep(500);
            }
            sleep(500);
            var hs = hits.slice(), ok = 0, j, k, best, bd, dx, dy, d;
            for (j = 0; j < targets.length; j++) {
                best = null; bd = 1e9;
                for (k = 0; k < hs.length; k++) {
                    dx = hs[k].x - targets[j].x; dy = hs[k].y - targets[j].y;
                    d = dx * dx + dy * dy;
                    if (d < bd) { bd = d; best = hs[k]; }
                }
                if (best && bd < 12 * 12) { ok++; log("PASS 角" + j + " 偏差" + Math.round(Math.sqrt(bd)) + "px"); }
                else log("FAIL 角" + j + " 最近" + (best ? Math.round(Math.sqrt(bd)) + "px" : "无落点"));
            }
            log(ok === targets.length ? "方向换算正确" : "方向换算有误（红绿错位）");
            redraw();
        } catch (e) { log("TEST-ERR " + e); }
    });
}

gW = floaty.window(
    '<vertical bg="#101418" padding="8">' +
    '<horizontal><text textSize="14sp" textColor="#7FD4FF" layout_weight="1" text="绿=目标 红=落点"/>' +
    '<button id="retest" text="再测" w="90" h="44"/><button id="quit" text="退出" w="90" h="44"/></horizontal>' +
    '<frame id="cap" layout_weight="1"><canvas id="board" layout_weight="1"/></frame>' +
    '</vertical>');
gW.setSize(device.width, device.height);
gW.cap.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, { onTouch: function (v, ev) {
    try {
        if (ev.getAction() === 0) {
            if (!gLoc) gLoc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
            gW.cap.getLocationOnScreen(gLoc);
            hits.push({ x: ev.getX() + gLoc[0], y: ev.getY() + gLoc[1] });
            redraw();
        }
    } catch (e) {}
    return true;
} }));
gW.board.on("draw", function (canvas) {
    var ts = targets, hs = hits, i, p;
    try { canvas.drawColor(colors.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR); } catch (e) {}
    var ox = 0, oy = 0;
    try {
        if (!gLoc) gLoc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
        gW.cap.getLocationOnScreen(gLoc);
        ox = gLoc[0]; oy = gLoc[1];
    } catch (e2) {}
    for (i = 0; i < ts.length; i++) {
        p = new Paint(); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(5); p.setColor(colors.GREEN);
        canvas.drawCircle(ts[i].x - ox, ts[i].y - oy, 60, p);
    }
    for (i = 0; i < hs.length; i++) {
        p = new Paint(); p.setColor(colors.RED);
        canvas.drawCircle(hs[i].x - ox, hs[i].y - oy, 22, p);
    }
});
uiWire(gW.retest, function () { runTest(); });
uiWire(gW.quit, function () {
    try { gW.close(); } catch (e) {}
    try { c.close(); } catch (e2) {}
    try { vt.stop(); } catch (e3) {}
    exit();
});

var lastRot = -1, lastW = 0, lastH = 0;
setInterval(function () {
    try {
        var r = vt.rot(), w = device.width, h = device.height;
        if (lastRot === -1) { lastRot = r; lastW = w; lastH = h; return; }
        if (r !== lastRot || w !== lastW || h !== lastH) {
            lastRot = r; lastW = w; lastH = h;
            try { gW.setSize(w, h); } catch (e) {}
            toast("转屏重测");
            runTest();
        }
    } catch (e2) {}
}, 1000);

runTest();
