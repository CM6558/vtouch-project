/* 触摸业务：只写触摸逻辑，其它全在 bundle 里。run 内直接 vt.finger()，不用传连接。 */
"use strict";
var vt = require("/sdcard/vtouch_bundle.js");
vt.run(function () {
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
});
