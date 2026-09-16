/**
 * circle_demo.js —— 「一直画圆」（AutoJs6，手机端直接跑，只 require 一个 JS 文件）
 *
 * 做什么：起 daemon（已在跑则复用）→ 拿一个虚拟槽 → 按住不放 → 按固定转速持续画圆，
 *   直到你手动停止（AutoJs6 的停止按钮 / 悬浮窗停止）。
 *   期间**你的真手指照常可用**：daemon 把物理手指和虚拟触点合成同一条触摸流。
 *
 * 停止时自动收尾：抬起虚拟触点 → 关连接 → 停 daemon（EVIOCGRAB 随进程退出释放，物理触摸回系统）。
 *
 * 跑法：本文件与 vtouch.js 一起放 /sdcard/，在 AutoJs6 里运行本文件。
 *   顶部 4 个参数可改：圆心比例、半径比例、转速、帧率。
 *
 * 预期行为（实测出来的，别期待错）：
 *   虚拟触点**持续在动**时，系统看到的是「一根正在拖动的手指」——所以这期间用真手指去
 *   **点击**控件可能不成立（滑动一般照常，坐标也会照走，所以画板类应用会看到轨迹）。
 *   想测「静止按住」：把 RPS 设 0、半径设 0 —— 坐标恒定 → 内核去重 → 按下之后系统零事件，
 *   此时真手指点/滑都正常。
 *
 * 前置：root（daemon 需要 /dev/uinput 与 EVIOCGRAB）。
 */
"use strict";
var vt = require("/sdcard/vtouch.js");

var CX_RATIO = 0.50;        // 圆心 x = 逻辑宽 × 这个比例
var CY_RATIO = 0.47;        // 圆心 y = 逻辑高 × 这个比例
var RADIUS_RATIO = 0.21;    // 半径   = 逻辑宽 × 这个比例（设 0 = 定点按住不动）
var RPS = 0.4;              // 每秒转几圈（设 0 = 不转）
var FPS = 60;               // 帧率：每帧一条 move
var SLOT = 0;               // 虚拟槽 0~9
var LOG_EVERY_MS = 2000;    // 状态日志间隔（用 log，不刷 toast）
var DRAIN_EVERY = 30;       // 每 N 帧收一次 daemon 回包（免得出站缓冲积压）

/* SDK 出现过两种签名：finger(conn, slot) 与 finger(slot)，两种都兼容 */
function mkFinger(c, slot) {
    try { return vt.finger(c, slot); } catch (e) { return vt.finger(slot); }
}

vt.start();                                  // 起 daemon（已在跑则复用）
var c = vt.connect();                        // 连接 + WS 握手

/* 坐标 = 竖屏逻辑坐标：用 daemon 自报的 res 校准（别用 device.width/height 猜） */
var res = c.cmd("res") || "";
var m = /res\s+(\d+)\s+(\d+)/.exec(res);
if (!m) throw new Error("拿不到 daemon 的逻辑尺寸: " + res);
var LW = parseInt(m[1], 10), LH = parseInt(m[2], 10);
var CX = Math.round(LW * CX_RATIO), CY = Math.round(LH * CY_RATIO);
var R = Math.round(LW * RADIUS_RATIO);

var f = mkFinger(c, SLOT);
var done = false;
function cleanup(tag) {
    if (done) return;
    done = true;
    try { f.up(); } catch (e) {}             // 一定抬指，别让屏幕上留一个按住的点
    try { c.close(); } catch (e) {}
    try { vt.stop(); } catch (e) {}          // 停 daemon：EVIOCGRAB 释放，物理触摸回系统
    toastLog(tag + " → 已抬指并停 daemon（物理触摸回到系统）");
}
events.on("exit", function () { cleanup("脚本退出"); });

toastLog("画圆：圆心(" + CX + "," + CY + ") 半径" + R +
    " · " + RPS + " 圈/秒 · " + FPS + " 帧/秒 · 槽 " + f.slot + "（真手指可同时使用）");
f.down(CX + R, CY);

var t0 = Date.now(), n = 0, lastLog = t0, fps = 0;
try {
    for (;;) {
        var t = (Date.now() - t0) / 1000;
        var ang = 2 * Math.PI * RPS * t;
        f.move(CX + R * Math.cos(ang), CY + R * Math.sin(ang));   // 一直画
        n++;
        if (n % DRAIN_EVERY === 0) c.drain();                     // 收掉 ok 回包
        var now = Date.now();
        if (now - lastLog >= LOG_EVERY_MS) {
            fps = n / ((now - t0) / 1000);
            log("画圆中 " + ((now - t0) / 1000).toFixed(0) + "s · 实测 " + fps.toFixed(1) + " 帧/秒" +
                " · 当前点 (" + Math.round(CX + R * Math.cos(ang)) + "," + Math.round(CY + R * Math.sin(ang)) + ")");
            lastLog = now;
        }
        var want = t0 + n * 1000 / FPS;                            // 按帧率节流
        var slp = want - Date.now();
        sleep(slp > 1 ? slp : 1);
    }
} catch (e) {
    cleanup("画不动了（连接断了？）:" + e);
}
