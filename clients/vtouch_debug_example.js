"use strict";
// 调试版：每步打点到 /sdcard/ex-debug.txt，看 8 行示例卡在哪。
var VTouch = plugins.load('org.vtouch.plugin');
function mark(s) { try { files.append("/sdcard/ex-debug.txt", Date.now() + " " + s + "\n"); } catch (e) {} }
mark("main start");
var vt = new VTouch();
mark("constructed");
threads.start(function () {
    mark("child start opened=" + vt.opened);
    try {
        for (var i = 0; i < 40; i++) {
            if (vt.opened) break;
            if (i % 4 === 0) mark("poll " + i + " opened=" + vt.opened);
            sleep(500);
        }
        mark("after-poll opened=" + vt.opened);
        vt.finger().tap(540, 1200);
        mark("tapped state=" + vt.finger(0).state());
        vt.close();
        mark("closed");
    } catch (e) {
        mark("ERR " + e);
        try { vt.close(); } catch (e2) {}
    }
    mark("child end");
    exit();
});
mark("main end");
