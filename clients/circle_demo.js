/**
 * circle_demo.js —— 「一直画圆」（AutoJs6，手机端直接跑，只 require 一个 JS 文件）
 *
 * 做什么：起 daemon（已在跑则复用）→ 拿一个虚拟槽 → 按住不放 → 按固定转速持续画圆，
 *   直到你手动停止（AutoJs6 停止按钮）。
 *   期间**你的真手指照常可用**：daemon 把物理手指和虚拟触点合成同一条触摸流。
 *
 * 停止时自动收尾：抬起虚拟触点 → 关连接 → 停 daemon（EVIOCGRAB 随进程退出释放，物理触摸回系统）。
 *
 * 跑法：本文件与 vtouch.js 一起放 /sdcard/，在 AutoJs6 里运行本文件。
 *   顶部几个参数可改：圆心比例、半径比例、转速、帧率、槽号。
 *
 * 与上一版的区别（上一版在手机上没画出来）：
 *   1. **不用 SDK 的 Finger 封装**，直接发协议命令。SDK 里 `finger(conn, slot)` 是两参签名，
 *      而旧版/其它版本的 `finger(slot)` 是一参 —— 用错不会抛错，只会静默发出 `down NaN …`
 *      （看起来在跑，其实什么都没注进去）。协议命令本来就三行，自己发最稳。
 *   2. 每条命令都**等 daemon 的真回包**（`ok` / `err …`），不是"发出去了就算成功"。
 *   3. 全流程写一份设备日志 `/sdcard/circle_demo.log`，出问题可以直接 `adb pull` 看。
 *
 * 预期行为（实测出来的，别期待错）：
 *   虚拟触点**持续在动**时，系统看到的是「一根正在拖动的手指」——这期间用真手指**点击**
 *   控件可能不成立（滑动一般照常，坐标照走，画板类应用会看到轨迹）。
 *   想测「静止按住」：把 RPS 设 0、RADIUS_RATIO 设 0 —— 坐标恒定 → 内核去重 → 按下之后
 *   系统零事件，此时真手指点/滑都正常。
 *
 * 前置：root（daemon 需要 /dev/uinput 与 EVIOCGRAB）。
 */
"use strict";
var vt = require("/sdcard/vtouch.js");

/* ---------- 可调参数 ---------- */
var CX_RATIO = 0.50;        // 圆心 x = 逻辑宽 × 这个比例
var CY_RATIO = 0.47;        // 圆心 y = 逻辑高 × 这个比例
var RADIUS_RATIO = 0.21;    // 半径   = 逻辑宽 × 这个比例（设 0 = 定点按住不动）
var RPS = 0.4;              // 每秒转几圈（设 0 = 不转）
var FPS = 60;               // 帧率：每帧一条 move
var SLOT = 0;               // 虚拟槽 0~9
var LOG_EVERY_MS = 2000;    // 状态日志间隔
var DRAIN_EVERY = 30;       // 每 N 帧收一次回包（免得出站缓冲积压）
var LOGCAT = "/sdcard/circle_demo.log";

/* ---------- 设备日志（adb pull /sdcard/circle_demo.log 就能看） ---------- */
function say(msg) {
    var line = "[" + new Date().toTimeString().substring(0, 8) + "] " + msg;
    log(line);
    try { files.append(LOGCAT, line + "\n"); } catch (e) { }
}
function resetlog() { try { files.write(LOGCAT, ""); } catch (e) { } }

/* ---------- 起 daemon：先试 SDK，不行就用 su 直接起（两种都报告真实结果） ---------- */
function pidof() {
    var r = shell("pidof vtouchd", true);
    return r && String(r.result || "").trim();
}
function startDaemon() {
    if (pidof()) { say("daemon 已在跑 pid=" + pidof() + "，复用"); return; }
    try { vt.start(); } catch (e) { say("SDK vt.start() 失败：" + e); }
    if (pidof()) { say("daemon 已由 SDK 起来 pid=" + pidof()); return; }
    say("改用 su 直接起 daemon");
    var cmd = "D=/data/local/tmp/vtouchd;[ -f $D ]||exit 11;"
        + "S=$(wm size 2>/dev/null);S=${S##*Physical size: };W=${S%%x*};H=${S##*x};"
        + "if [ $W -gt $H ];then T=$W;W=$H;H=$T;fi;"
        + "kill -9 $(pidof vtouchd) 2>/dev/null;"
        + "nohup $D -w $W -h $H -p 27183 >/data/local/tmp/vtouchd.log 2>&1 </dev/null & echo started";
    say("su 启动: " + shell("su -c '" + cmd.replace(/'/g, "'\\''") + "'", true));
    for (var i = 0; i < 30 && !pidof(); i++) sleep(150);
    if (!pidof()) {
        var tail = shell("su -c 'tail -5 /data/local/tmp/vtouchd.log'", true);
        throw new Error("daemon 没起来；日志尾部：" + (tail && tail.result));
    }
    say("daemon 已起来 pid=" + pidof());
}

/* ---------- 主流程 ---------- */
resetlog();
say("=== circle_demo 启动 ===");
startDaemon();

var c = vt.connect();
say("已连接 WS");

var res = c.cmd("res") || "";
say("res → " + res);
var m = /res\s+(\d+)\s+(\d+)/.exec(res);
if (!m) throw new Error("拿不到 daemon 的逻辑尺寸: " + res);
/* 数值化一律走 num()：Rhino 里 Math.round 之类可能给出包装对象，直接拼进字符串会变成
 * "[object Object]"，拼出的命令就是 down 0 720[object Object] 1489 —— daemon 只能回 err point。 */
function num(v) { var x = Number(v); return isFinite(x) ? Math.round(x) : NaN; }
var LW = num(m[1]), LH = num(m[2]);
var CX = num(LW * CX_RATIO), CY = num(LH * CY_RATIO), RAD = num(LW * RADIUS_RATIO);
if (!isFinite(LW) || !isFinite(LH) || !isFinite(CX) || !isFinite(CY) || !isFinite(RAD))
    throw new Error("尺寸/圆心/半径算出来不是有限数: LW=" + LW + " LH=" + LH + " CX=" + CX + " CY=" + CY + " RAD=" + RAD);
say("逻辑尺寸 " + LW + "x" + LH + " → 圆心(" + CX + "," + CY + ") 半径" + RAD +
    " · " + RPS + " 圈/秒 · " + FPS + " 帧/秒 · 槽 " + SLOT);

var done = false;
function cleanup(tag) {
    if (done) return;
    done = true;
    try { say("up " + SLOT + " → " + c.cmd("up " + SLOT, 800)); } catch (e) { }
    try { c.close(); } catch (e) { }
    try { vt.stop(); } catch (e) { }
    say(tag + " → 已抬指并停 daemon（物理触摸回到系统）");
    try { toast(tag); } catch (e) { }
}
events.on("exit", function () { cleanup("脚本退出"); });

/* 第一条命令就用 cmd（等真回包）——发出去了不等于注进去了 */
var downCmd = "down " + SLOT + " " + num(CX + RAD) + " " + num(CY);
say("发出: " + downCmd);                       // 原样记下来：命令长什么样，一眼就能看
var r1 = c.cmd(downCmd, 1500);
say("down → " + r1);
if (!r1 || r1.indexOf("ok") !== 0) { cleanup("down 被拒：" + r1); throw new Error("down 被拒: " + r1); }

var t0 = Date.now(), n = 0, lastLog = t0, errs = 0;
try {
    for (;;) {
        var t = (Date.now() - t0) / 1000;
        var ang = 2 * Math.PI * RPS * t;
        c.send("move " + SLOT + " " + num(CX + RAD * Math.cos(ang)) + " " + num(CY + RAD * Math.sin(ang)));
        n++;
        if (n % DRAIN_EVERY === 0) {
            /* 顺手核对回包：daemon 对每条命令都回一行，出现 err 就记账 */
            try {
                for (var s = c.recv(); s !== null; s = c.recv()) {
                    if (s.indexOf("err") === 0) { errs++; if (errs < 4) say("!! daemon 回 " + s); }
                }
            } catch (e) { throw new Error("连接断开：" + e); }
        }
        var now = Date.now();
        if (now - lastLog >= LOG_EVERY_MS) {
            say("画圆中 " + ((now - t0) / 1000).toFixed(0) + "s · " + (n / ((now - t0) / 1000)).toFixed(1) +
                " 帧/秒 · err " + errs + " 次 · 当前点 (" +
                num(CX + RAD * Math.cos(ang)) + "," + num(CY + RAD * Math.sin(ang)) + ")");
            lastLog = now;
        }
        var slp = t0 + n * 1000 / FPS - Date.now();
        sleep(slp > 1 ? slp : 1);
    }
} catch (e) {
    cleanup("画不动了：" + e);
}
