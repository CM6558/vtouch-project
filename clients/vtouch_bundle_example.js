/* 触摸业务：只写触摸逻辑，其它（唤醒/保活/启动/连接/关闭/报错）全在 bundle 里。 */
"use strict";
var vt = require("/sdcard/vtouch_bundle.js");
vt.run(function (c) {
    vt.finger(c).tap(540, 1200);
    var a = vt.finger(c, 0), b = vt.finger(c, 1);
    vt.frame(c, [
        { slot: a.slot, state: "down", x: 500, y: 1200 },
        { slot: b.slot, state: "down", x: 900, y: 1200 }
    ]);
    sleep(200);
    vt.frame(c, [
        { slot: a.slot, state: "up", x: 500, y: 1200 },
        { slot: b.slot, state: "up", x: 900, y: 1200 }
    ]);
    if (a.state() !== "up" || b.state() !== "up") throw new Error("手指未释放");
});
