/**
 * touchback_demo.js —— 「回触示例」（AutoJs6，最小版 SDK）
 *
 * 链路：物理手指 → daemon 抓取 → pev/region_ev 推给本脚本 → 命中区域 → **回触**（注入一路虚拟槽）
 *
 * 为什么不会自己触发自己（Plan B 的两道门）：
 *   1) pev 只报物理手指 —— 客户端自己注入的轨迹不会被回灌（§4.1）；
 *   2) region_ev 的判定只在区域线程里跑，且 `if (ev.virt) continue`（§4.4）——
 *      虚拟触点进得了转发队列、进不了判定。
 *   本脚本里的 SELFTEST 把这条做成可断言的：回触后 300ms 内收到的 region_ev 都算自激。
 *
 * 与旧架构的差别（值得注意）：旧版回调读线程里带 sleep 的动作会把事件读堵住；现在 daemon 的出站
 *   队列（§4.5）让「客户端慢」只是堆队列，注入路径照常跑 —— 所以这里的回触可以就地做，
 *   不需要 threads.start（但为了演示「读线程不该被业务动作拖住」，仍丢给子线程）。
 *
 * 前置：daemon 已在跑（vt.start() 会起；root 必需）。跑法：AutoJs6 里直接运行本文件。
 */
"use strict";
var vt = require("/sdcard/vtouch.js");

var REGION_ID = "tapR";             // 右半屏中央那块（下面自动建）
var SELFTEST = true;
var PEDBG = false;                  // true = 连物理轨迹 pev 一起打日志

vt.start();
var c = vt.connect();
var res = c.cmd("res") || "";
var m = /res\s+(\d+)\s+(\d+)/.exec(res);
if (!m) throw new Error("拿不到 daemon 的逻辑尺寸: " + res);
var LW = parseInt(m[1], 10), LH = parseInt(m[2], 10);

c.cmd("region clear");
c.cmd("region add " + REGION_ID + " 0 "
    + Math.round(LW / 2) + " " + Math.round(LH / 3) + " "
    + Math.round(LW - LW / 8) + " " + Math.round(LH * 2 / 3) + " 1");
c.cmd("sub all");                    // 物理轨迹 + 区域事件都要（回触靠 region_ev 触发）

var injecting = 0;
function busy(fn) {                  // 回触丢子线程：读循环只负责收事件
    threads.start(function () { try { fn(); } catch (e) { toastLog("回触失败: " + e); } });
}

for (;;) {
    var line = null;
    try { line = c.recv(); } catch (e) { toastLog("连接断开: " + e); break; }
    if (!line) { sleep(5); continue; }

    var p = String(line).split(/\s+/);
    if (p[0] === "pev") {
        if (PEDBG) log("pev " + line);
        continue;
    }
    if (p[0] !== "region_ev" || p.length < 6) continue;

    var id = p[1], ev = p[2], slot = parseInt(p[3], 10), x = parseInt(p[4], 10), y = parseInt(p[5], 10);
    if (SELFTEST && injecting > 0) { toastLog("[自激!] 回触期间收到 region_ev：" + line); continue; }
    if (id !== REGION_ID) continue;

    if (ev === "down") {
        toastLog("按下 " + id + " @ " + x + "," + y + " → 回触：点屏幕上方正中");
        injecting++;
        busy(function () {
            try { vt.finger(5).down(LW / 2, LH / 5).move(LW / 2, LH / 5 + 6).up(); }
            finally { sleep(300); injecting--; }        // 300ms 观察窗：这段时间不该有 region_ev
        });
    } else if (ev === "up") {
        toastLog("抬起 " + id + " @ " + x + "," + y);
    } else {
        log("region_ev " + ev + " @" + x + "," + y);    // enter/exit/move 只记日志，免得刷屏
    }
}
