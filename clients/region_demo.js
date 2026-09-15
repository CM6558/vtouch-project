/**
 * region_demo.js —— 「五事件示例」（AutoJs6，最小版 SDK：只 require 一个 JS 文件）
 *
 * 做什么：建一个覆盖屏幕中央的区域，订阅区域通道，把 daemon 推来的 region_ev 逐条打出来。
 *   down / enter / move / exit / up 五种事件各是什么时候来的，看日志就明白：
 *     手指在区域里按下            → down（并记住「这一按命中过」，用于抬起时判 up）
 *     手指从区域外划进来          → enter
 *     手指在区域内移动（每帧变化）→ move
 *     手指划出区域                → exit
 *     手指抬起（且这次按下命中过）→ up
 *
 * 关键前提：区域判定只认**物理手指**（真手指/fake_touch.sh 伪造的物理事件）。
 *   本脚本注入的虚拟触点不会产生 region_ev —— 否则「回触」就会自己触发自己，无限连击。
 *   想看这条，把下面 SELF_CHECK 打开：注入期间收到的 region_ev 一律会被记为「自激」。
 *
 * 前置：daemon 已在跑（vt.start() 会起；root 必需）。
 * 跑法：AutoJs6 里直接运行本文件；手指在屏幕中央按一下试试。
 */
"use strict";
var vt = require("/sdcard/vtouch.js");

var SELF_CHECK = true;              // 注入期间收到 region_ev 就报警（应该是 0 条）
var REGION_ID = "s3";

vt.start();
var c = vt.connect();

/* 坐标 = 竖屏逻辑坐标：用 daemon 的 res 校准，别用 device.width/height 猜 */
var res = c.cmd("res") || "";
toastLog(res);
var m = /res\s+(\d+)\s+(\d+)/.exec(res);
if (!m) throw new Error("拿不到 daemon 的逻辑尺寸: " + res);
var LW = parseInt(m[1], 10), LH = parseInt(m[2], 10);

/* 区域唯一归属是 daemon 的表：先清空再下发（重复 add 同 id 是原地更新） */
toastLog(c.cmd("region clear"));
toastLog(c.cmd("region add " + REGION_ID + " 0 "       // 0 = 矩形（1 = 圆形：cx cy r 0）
    + Math.round(LW / 4) + " " + Math.round(LH / 4) + " "
    + Math.round(LW * 3 / 4) + " " + Math.round(LH * 3 / 4) + " 1"));
toastLog(c.cmd("sub region"));                          // 只订区域事件（不订物理轨迹）

var injecting = false, stats = {};
function parseEv(line) {                                 // region_ev <id> <ev> <slot> <x> <y>
    var p = String(line).split(/\s+/);
    if (p[0] !== "region_ev" || p.length < 6) return null;
    return { id: p[1], ev: p[2], slot: parseInt(p[3], 10), x: parseInt(p[4], 10), y: parseInt(p[5], 10) };
}

toastLog("就绪：中央区域 " + REGION_ID + "（" + LW + "x" + LH + "）—— 按一下、划进划出试试");
for (;;) {
    var line = null;
    try { line = c.recv(); } catch (e) {                       // 服务端关闭（被新连接踢掉等）
        toastLog("连接断开: " + e);
        break;
    }
    if (line) {
        var h = parseEv(line);
        if (h) {
            stats[h.ev] = (stats[h.ev] || 0) + 1;
            toastLog("[region_ev] " + h.ev + "  id=" + h.id + "  slot=" + h.slot + "  @" + h.x + "," + h.y
                + "   (down " + (stats.down || 0) + " / enter " + (stats.enter || 0)
                + " / move " + (stats.move || 0) + " / exit " + (stats.exit || 0)
                + " / up " + (stats.up || 0) + ")");
            if (SELF_CHECK && injecting) toastLog("[自激!] 注入期间收到 region_ev：" + line);
        }
    } else {
        sleep(5);                                              // recv() 非阻塞，空转让一下
    }
}
