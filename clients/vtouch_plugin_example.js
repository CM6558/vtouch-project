/*
 * vtouch 项目插件示例：通过 AutoJs6 插件机制加载 SDK，多脚本共享一份实现。
 *
 * 项目结构（AutoJs6 中把 /sdcard/vtouch-merge/ 当作项目打开）：
 *   /sdcard/vtouch-merge/          <- 项目根目录
 *   ├── plugins/
 *   │   └── vtouch.js              <- 项目插件（本 SDK，module.exports = VTouch）
 *   └── vtouch_plugin_example.js   <- 本脚本
 *
 * 加载方式（推荐，需从项目根目录运行脚本）：
 *   var VTouch = plugins.load('vtouch');      // 或 plugins.load('vtouch.js')
 *
 * 通用回退（不依赖"项目"识别，任何脚本可直接 require 绝对路径）：
 *   var VTouch = require('/sdcard/vtouch-merge/plugins/vtouch.js');
 *
 * 使用：
 *   var vt = new VTouch(options).connect(onReady);
 *   options: url(默认 ws://127.0.0.1:27183), width/height(默认 device.width/height),
 *            timeout(默认 15000ms)
 *   connect 会自动检查并启动 vtouch 服务（root），失败自动重试；
 *   脚本退出时 events.on("exit") 自动 close + stopService 释放物理触摸独占。
 *
 * Finger API（f 为 client.finger() 返回的 Finger 对象）：
 *   client.finger()       自动分配空闲 slot 0~9
 *   client.finger(2)      使用指定 slot 2
 *   f.slot                实际 slot 编号
 *   f.down(x, y)          按下并保持
 *   f.move(x, y)          移动（未按下时跳过并告警）
 *   f.up()                抬起并释放 slot
 *   f.tap(x, y, ms)       点击，ms 默认 60
 *   f.swipe(x1,y1,x2,y2,durationMs)   滑动，durationMs 默认 300ms
 *   f.frame(state,x,y)    当前手指的 down/move/up 帧操作
 *   f.state()             返回 down 或 up
 *   f.cancel()            清理本地状态，不会抬起触点
 *
 * VTouch API：
 *   client.frame(points)  多指同帧操作 [{slot,state,x,y}, ...]
 *   client.gesture(frames)连续提交多帧
 *   client.pinch(cx,cy,startGap,endGap)   双指缩放
 *   client.reset()        释放全部模拟触点
 *   client.close()        关闭连接
 *
 * 坐标使用 device.width/device.height 的逻辑屏幕坐标，服务端负责转换。
 * sleep(ms) 只阻塞当前脚本线程；如需阻塞等待，放进 threads.start 中执行，
 * 避免饿死 WebSocket 回调。
 */
"use strict";

/* ---- 加载插件 ---- */
var VTouch = plugins.load('vtouch');
// 如遇 "插件不存在"，改用通用回退：
// var VTouch = require('/sdcard/vtouch-merge/plugins/vtouch.js');

var vt = new VTouch().connect(function (client) {
    threads.start(function () {
        var f = client.finger();
        // var f2 = client.finger();
        f.swipe(200, 200, 1500, 2000, 1000);
        // f2.swipe(200, 2000, 1500, 2000, 1000);

        /* 样例 1：点击 */
        // f.tap(client.width / 2, client.height / 2, 60);

        /* 样例 2：按下、等待、移动、抬起 */
        // f.down(client.width / 2, client.height / 2);
        // sleep(1000);
        // f.move(client.width / 2 + 50, client.height / 2);
        // f.up();

        /* 样例 3：单指滑动 */
        // f.swipe(200, 2000, 900, 2000, 1000);

        /* 样例 4：显式 slot */
        // var f2 = client.finger(2);
        // f2.down(720, 1584).move(760, 1584).up();

        /* 样例 5：两个 Finger 同帧移动 */
        // var f0 = client.finger(0), f1 = client.finger(1);
        // f0.down(500, 1200); f1.down(900, 1200);
        // client.frame([
        //     {slot: f0.slot, state: "move", x: 450, y: 1200},
        //     {slot: f1.slot, state: "move", x: 950, y: 1200}
        // ]);
        // f0.up(); f1.up();

        /* 样例 6：双指缩放 */
        // client.pinch(client.width / 2, client.height / 2, 200, 1000);

        /* 样例 7：手势帧序列 */
        // client.gesture([
        //     [
        //         {slot: 0, state: "down", x: 500, y: 1200},
        //         {slot: 1, state: "down", x: 900, y: 1200}
        //     ],
        //     [
        //         {slot: 0, state: "move", x: 450, y: 1200},
        //         {slot: 1, state: "move", x: 950, y: 1200}
        //     ],
        //     [
        //         {slot: 0, state: "up", x: 450, y: 1200},
        //         {slot: 1, state: "up", x: 950, y: 1200}
        //     ]
        // ]);

        /* 样例 8：异常清理（SIGKILL 强杀会跳过 exit handler，业务里显式调用） */
        // client.reset();
        // client.close();
        // vt.stopService();
    });
});
