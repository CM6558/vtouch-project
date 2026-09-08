/* =============================================================
 * vtouch 同步式 SDK 使用示例
 *
 * 核心：业务代码全部放进 vt.run(function(){...}) 子线程，
 *      用同步写法（connect/不带回调的 tap/swipe/sleep），
 *      主线程保持空闲处理 WebSocket 事件。
 *
 * 模式：
 *   var vt = new VTouch();
 *   vt.run(function () {
 *       vt.connect();                      // 阻塞到就绪
 *       var f = vt.finger();
 *       f.tap(x, y, ms);                   // 同步点击
 *       f.swipe(x1,y1,x2,y2,ms);           // 同步滑动
 *       f.down(x, y); sleep(1000); f.up(); // 按下-等待-抬起
 *       vt.stop();                          // 关闭连接+停止服务
 *   });
 *
 * Finger:  finger() 自动分配空闲 slot 0~9；finger(n) 指定 slot
 *   f.down/move/up/tap/swipe/frame(state,x,y)/state  全部同步
 * VTouch:  connect() 同步阻塞连接；reset() 释放全部触点；
 *          stop() 关闭连接并停止服务（释放 EVIOCGRAB）
 * ============================================================= */
"use strict";

// 引入 SDK（三选一，看你用哪种形态）
var VTouch = require('/sdcard/vtouch-merge/vtouch_sdk.js');   // 单文件
// var VTouch = plugins.load('vtouch');                       // 项目插件
// var VTouch = plugins.load('org.vtouch.plugin');            // 应用插件(APK)

var vt = new VTouch();
vt.onError = function (msg) { log('[vtouch] server error: ' + msg); };

vt.run(function () {
    try {
        vt.connect();   // 同步阻塞，连接失败自动重试/启动服务，超时抛异常
        log('[vtouch] connected in business thread');

        var f = vt.finger();
        f.swipe(200, 200, 1500, 2000, 1000);
        f.tap(vt.width / 2, vt.height / 2, 60);

        vt.stop();
        log('[vtouch] stopped');
    } catch (e) {
        log('[vtouch] test failed: ' + e);
        vt.stop();
    }
});