/*
 * vtouch 精简示例：Finger 对象 + 原子帧，时序由 sleep 控制。
 *
 * 加载（应用插件 APK，任意脚本可用）：
 *   var VTouch = plugins.load('org.vtouch.plugin');
 * 回退（项目插件 / 直接 require）：
 *   var VTouch = plugins.load('vtouch');
 *   var VTouch = require('/sdcard/vtouch-merge/plugins/vtouch.js');
 *
 * 规则：tap / swipe / ready 内部有 sleep，必须在 threads.start() 业务线程里调，
 * 主线程留给 WebSocket 事件循环。退出自动清理（close + stopService，
 * new VTouch({autoStop: false}) 可关掉后者）。
 *
 * Finger（f = vt.finger() 自动取空闲 slot，vt.finger(2) 指定 slot）：
 *   f.down(x, y) / f.move(x, y) / f.up()
 *   f.tap(x, y, ms)                  点击，ms 默认 60
 *   f.swipe(x1, y1, x2, y2, ms)      滑动，默认 300ms
 *   f.frame(state, x, y) / f.state() 单指帧 / 查状态
 * VTouch：
 *   vt.frame([{slot, state, x, y}])  多指同帧原子提交（单次 SYN_REPORT）
 *   vt.ready() / vt.reset() / vt.close()
 */
var VTouch = plugins.load('org.vtouch.plugin');
// var VTouch = require('/sdcard/vtouch-merge/plugins/vtouch.js');

threads.start(function () {
    var vt = new VTouch();
    vt.ready();                                    // 等 WS 连上（业务线程内）

    vt.finger().tap(540, 1200, 60);                // 点击
    vt.finger().swipe(200, 200, 1500, 2000, 500);  // 滑动

    var a = vt.finger(0), b = vt.finger(1);        // 双指：同帧按下
    vt.frame([
        { slot: a.slot, state: "down", x: 500, y: 1200 },
        { slot: b.slot, state: "down", x: 900, y: 1200 }
    ]);
    sleep(300);
    vt.frame([
        { slot: a.slot, state: "up", x: 500, y: 1200 },
        { slot: b.slot, state: "up", x: 900, y: 1200 }
    ]);

    vt.close();
    exit();
});
