/*
 * vtouch 插件示例（v2）：自动连接 + 一行一个动作。
 *
 * 加载方式（应用插件 APK，任意脚本可用）：
 *   var VTouch = plugins.load('org.vtouch.plugin');
 *   未安装 APK 时回退项目插件/require：
 *   var VTouch = plugins.load('vtouch');                    // 项目插件
 *   var VTouch = require('/sdcard/vtouch-merge/plugins/vtouch.js');
 *
 * v2 新写法（推荐）：
 *   var vt = new VTouch();        // 自动连接（服务未启动自动拉起 vtouchmerge + vtouchws）
 *   vt.tap(540, 1200);            // 一行一个动作，无需回调、无需 threads.start
 *   vt.swipe(200, 200, 900, 2000, 1000);
 *
 * 退出自动清理：events.on("exit") 自动 close + stopService（autoStop:false 可关）。
 *
 * 插件 Java API 优先（APK 模式下）：
 *   胶水层覆盖 startService/stopService 走插件实例的 Java 实现
 *   （root 进程内拉起 vtouchmerge/vtouchws），失败自动回退 shell。
 *
 * Finger API（f 为 vt.finger() 返回的 Finger 对象）：
 *   vt.finger()         自动分配空闲 slot 0~9
 *   vt.finger(2)        使用指定 slot 2
 *   f.down(x, y) / f.move(x, y) / f.up()
 *   f.tap(x, y, ms)     点击，ms 默认 60
 *   f.swipe(x1,y1,x2,y2,durationMs)  滑动，默认 300ms，内部线程插值
 *   f.hold(ms, fn) / f.press(x, y, ms, fn)
 *   f.frame(state,x,y) / f.state() / f.cancel()
 *
 * VTouch API：
 *   vt.tap / vt.swipe / vt.down / vt.move / vt.up   便捷单指（自动分配手指）
 *   vt.frame(points)    多指同帧原子提交（单次 SYN_REPORT）
 *   vt.gesture(frames[, durationMs])  帧序列，可选总时长
 *   vt.pinch(cx,cy,startGap,endGap,durationMs)  双指缩放
 *   vt.reset() / vt.close() / vt.onError
 */

var VTouch = plugins.load('org.vtouch.plugin');
// 未安装 APK 时回退：
// var VTouch = require('/sdcard/vtouch-merge/plugins/vtouch.js');

var vt = new VTouch();      // 自动连接
// vt.onError = function (msg) { log("[vtouch] server error: " + msg); };

/* 样例 1：点击 */
vt.tap(540, 1200, 60);

/* 样例 2：滑动 */
vt.swipe(200, 200, 1500, 2000, 1000);

/* 样例 3：按下、等待、移动、抬起（sleep 放子线程） */
// threads.start(function () {
//     vt.down(540, 1200);
//     sleep(1000);
//     vt.move(590, 1200);
//     vt.up();
// });

/* 样例 4：双指缩放 */
// vt.pinch(vt.width / 2, vt.height / 2, 200, 1000, 800);

/* 样例 5：多指原子帧 */
// vt.frame([
//     {slot: 0, state: "down", x: 500, y: 1200},
//     {slot: 1, state: "down", x: 900, y: 1200}
// ]);
// vt.frame([
//     {slot: 0, state: "up", x: 500, y: 1200},
//     {slot: 1, state: "up", x: 900, y: 1200}
// ]);

/* 样例 6：hold / press */
// var f = vt.finger();
// f.down(300, 500).hold(1000, function (finger) { finger.move(420, 620).up(); });
// f.press(700, 900, 250, function (finger) { log(finger.state()); });
