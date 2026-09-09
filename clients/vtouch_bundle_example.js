/* 触摸业务写法二（同步阻塞，主线程直写到底，不用 threads，不用回调）：
 * 裸 Socket 全是阻塞 I/O，主线程调也安全；脚本跑完自然结束。
 * 回调写法照样可用：vt.run(function () { vt.finger().tap(540, 1200); });
 */
"use strict";
var vt = require("/sdcard/vtouch_bundle.js");
vt.ensure();
var c = vt.connect();
try {
    vt.finger().tap(540, 1200);
    var a = vt.finger(0), b = vt.finger(1);
    vt.frame([
        { slot: a.slot, state: "down", x: 500, y: 1200 },
        { slot: b.slot, state: "down", x: 900, y: 1200 }
    ]);
    sleep(200);
    vt.frame([
        { slot: a.slot, state: "up", x: 500, y: 1200 },
        { slot: b.slot, state: "up", x: 900, y: 1200 }
    ]);
    if (a.state() !== "up" || b.state() !== "up") throw new Error("手指未释放");
} finally {
    try { c.close(); } catch (e) {}
    vt.stop();
}
