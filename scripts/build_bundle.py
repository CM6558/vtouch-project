#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_bundle.py — 生成单文件自包含包 clients/vtouch_bundle.js。

组成：内嵌 vtouchd 二进制(base64) + 一层薄函数。传输层不用 AutoJs6 的
OkHttp WebSocket（其回调派发会间歇性卡死），改用 java.net.Socket 手写
最小 WS 客户端（握手 + masked 文本帧 + 解帧），裸 Socket 次次直通。
手机上只要这一个 JS 文件。

用法: python scripts/build_bundle.py [--check]
"""
import base64
import gzip
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "vtouchd"
OUT = ROOT / "clients" / "vtouch_bundle.js"
UI_DIR = ROOT / "build" / "ui"
UI_SO = UI_DIR / "libtestimgui.so"
UI_DEX = UI_DIR / "classes.dex"

CORE = '''/* ============================================================================
 * VTouch 单文件包：二进制内嵌（vtouchd + ImGui 面板 dex/so）+ 一层薄函数。由 scripts/build_bundle.py 生成，勿手改。
 * 手机上只要这一个文件。首次运行自动释放二进制（需 root，一次），之后直启。
 * 阻塞调用只能在业务线程调，主线程调会卡死 Looper。
 * ============================================================================
 */
"use strict";

var VTOUCH_BIN = "/data/local/tmp/vtouchd";
var VTOUCH_HOST = "127.0.0.1";
var VTOUCH_PORT = 27183;

/* 当前连接：connect() 成功后自动设定，close()/stop() 后清除。
 * finger/frame/reset 直接用它，run 之外不用传参。 */
var CURR = null;
function vtouchCur() {
    if (!CURR) throw new Error("无当前连接：先调 vt.ensure() + vt.connect()，或在 vt.run 内调用");
    return CURR;
}

/* 后端幂等启动：pid 存活直接返回；缺二进制则释放；然后拉起（不等端口）。
 * daemon 坐标系恒为竖屏物理（wm size portrait）：W/H 归一化 min/max，横屏开机也不错位。 */
function vtouchEnsure() {
    /* 面板就是 daemon（grab + WS 27183 + regions.conf）：已在跑就别再起第二个抢端口。 */
    if (vtouchUiAlive()) return;
    var r = shell("B=/data/local/tmp/vtouch-runtime;D=" + VTOUCH_BIN + ";"
        + "[ -f $B/vtouchd.pid ]&&kill -0 $(cat $B/vtouchd.pid 2>/dev/null) 2>/dev/null&&exit 0;"
        + "[ -x $D ]||exit 11;"
        + "mkdir -p $B;kill -9 $(pidof vtouchd);rm -f $B/vtouchd.pid;"
        + "S=$(wm size 2>/dev/null);S=${S##*Physical size: };W=${S%%x*};H=${S##*x};"
        + "if [ $W -gt $H ];then T=$W;W=$H;H=$T;fi;"
        + "nohup $D -w $W -h $H -p " + VTOUCH_PORT + " >$B/vtouchd.log 2>&1 </dev/null&echo $!>$B/vtouchd.pid", true);
    if (r && r.code === 11) {
        vtouchInstall();
        return;
    }
    if (r && r.code !== 0) throw new Error("启动失败: " + (r.error || r.result || r.code));
}

/* 缺二进制时：解码内嵌包 → root cp → 再走一遍 ensure（此时必命中文件分支）。 */
function vtouchInstall() {
    log("[vtouch] 释放二进制 (" + VTOUCH_BIN_SIZE + "B)...");
    var bytes = android.util.Base64.decode(VTOUCH_BIN_B64, android.util.Base64.DEFAULT);
    var stage = context.getFilesDir().getAbsolutePath() + "/vtouchd.stage";
    var fos = new java.io.FileOutputStream(stage);
    try { fos.write(bytes); } finally { try { fos.close(); } catch (e) {} }
    var cp = shell("cp -f '" + stage + "' " + VTOUCH_BIN + " && chmod 755 " + VTOUCH_BIN, true);
    if (!cp || cp.code !== 0) throw new Error("释放失败: " + ((cp && (cp.error || cp.result)) || "unknown"));
    log("[vtouch] 二进制就绪");
    var r2 = shell("B=/data/local/tmp/vtouch-runtime;D=" + VTOUCH_BIN + ";"
        + "mkdir -p $B;kill -9 $(pidof vtouchd);rm -f $B/vtouchd.pid;"
        + "S=$(wm size 2>/dev/null);S=${S##*Physical size: };W=${S%%x*};H=${S##*x};"
        + "if [ $W -gt $H ];then T=$W;W=$H;H=$T;fi;"
        + "nohup $D -w $W -h $H -p " + VTOUCH_PORT + " >$B/vtouchd.log 2>&1 </dev/null&echo $!>$B/vtouchd.pid", true);
    if (r2 && r2.code !== 0) throw new Error("启动失败: " + (r2.error || r2.result || r2.code));
}

/* ---- 最小 WS 客户端（裸 Socket，不经 OkHttp） ---- */
function vtouchWsKey() {
    var b = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, 16);
    new java.util.Random().nextBytes(b);
    return android.util.Base64.encodeToString(b, android.util.Base64.NO_WRAP);
}
function vtouchWsAccept(key) {
    var md = java.security.MessageDigest.getInstance("SHA-1");
    var d = md.digest(new java.lang.String(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").getBytes("UTF-8"));
    return android.util.Base64.encodeToString(d, android.util.Base64.NO_WRAP);
}
function vtouchWriteAll(out, bytes) {
    out.write(bytes);
    out.flush();
}
/* 发包全局锁：读线程的 ping 和业务线程的触摸包可能并发写同一 socket。 */
var SEND_LOCK = threads.lock();
function vtouchReadFull(ins, n) {
    var buf = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n);
    var off = 0;
    while (off < n) {
        var k = ins.read(buf, off, n - off);
        if (k < 0) throw new Error("连接断开");
        off += k;
    }
    return buf;
}
function vtouchReadLine(ins) {
    var sb = new java.lang.StringBuilder();
    for (;;) {
        var b = ins.read();
        if (b < 0) throw new Error("连接断开");
        if (b === 10) break;
        if (b !== 13) sb.append(chr(b));
    }
    return sb.toString();
}
function chr(c) { return String.fromCharCode(c); }
/* Java byte 有符号：Rhino 侧 128~255 必须折成负数再写入 byte[]。 */
function jb(v) { v = v & 255; return v > 127 ? v - 256 : v; }

/* 连接并完成握手。必须在业务线程调（阻塞读）。daemon 刚拉起时端口滞后，
 * 这里小步重试直到超时。返回 conn{send,recv,close}。 */
function vtouchConnect(timeout) {
    timeout = timeout || 10000;
    var t0 = Date.now(), lastErr = null;
    for (;;) {
        try { return vtouchConnectOnce(timeout); }
        catch (e) {
            lastErr = e;
            if (Date.now() - t0 > timeout) throw new Error("ws 未连接: " + lastErr);
            sleep(300);
        }
    }
}
function vtouchConnectOnce(timeout) {
    var sock = new java.net.Socket();
    sock.connect(new java.net.InetSocketAddress(VTOUCH_HOST, VTOUCH_PORT), 3000);
    sock.setSoTimeout(timeout);
    var out = sock.getOutputStream(), ins = sock.getInputStream();
    var key = vtouchWsKey();
    var req = "GET / HTTP/1.1\\r\\nHost: " + VTOUCH_HOST + ":" + VTOUCH_PORT + "\\r\\n"
        + "Upgrade: websocket\\r\\nConnection: Upgrade\\r\\n"
        + "Sec-WebSocket-Key: " + key + "\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n";
    vtouchWriteAll(out, new java.lang.String(req).getBytes("UTF-8"));
    var status = vtouchReadLine(ins);
    if (status.indexOf("101") < 0) { try { sock.close(); } catch (e) {} throw new Error("握手失败: " + status); }
    var accept = vtouchWsAccept(key), ok = false, line;
    for (;;) {
        line = vtouchReadLine(ins);
        if (line.length === 0) break;
        if (line.toLowerCase().indexOf("sec-websocket-accept") === 0 && line.indexOf(accept) >= 0) ok = true;
    }
    if (!ok) { try { sock.close(); } catch (e) {} throw new Error("握手 accept 不匹配"); }
    var conn = { sock: sock, out: out, ins: ins, mask: new java.util.Random() };
    conn.send = function (text) {
        SEND_LOCK.lock();
        try {
        var data = new java.lang.String(text).getBytes("UTF-8");
        var n = data.length, i, m = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, 4);
        conn.mask.nextBytes(m);
        var h = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n < 126 ? 2 : 4);
        h[0] = jb(0x81);
        if (n < 126) { h[1] = jb(0x80 | n); }
        else { h[1] = jb(0x80 | 126); h[2] = jb(n >> 8); h[3] = jb(n); }
        vtouchWriteAll(conn.out, h);
        vtouchWriteAll(conn.out, m);
        var masked = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n);
        for (i = 0; i < n; i++) masked[i] = jb(data[i] ^ m[i & 3]);
        vtouchWriteAll(conn.out, masked);
        } finally { SEND_LOCK.unlock(); }
    };
    /* 读一条服务端文本消息；无数据时返回 null（不阻塞）。 */
    conn.recv = function () {
        if (conn.ins.available() < 2) return null;
        var h = vtouchReadFull(conn.ins, 2);
        var len = h[1] & 127, i;
        if (len === 126) { var e = vtouchReadFull(conn.ins, 2); len = ((e[0] & 255) << 8) | (e[1] & 255); }
        else if (len === 127) throw new Error("帧过大");
        if ((h[0] & 15) === 8) { try { conn.sock.close(); } catch (e2) {} throw new Error("服务端关闭"); }
        var p = vtouchReadFull(conn.ins, len);
        var cs = [];
        for (i = 0; i < len; i++) cs.push(String.fromCharCode(p[i] & 255));
        return decodeURIComponent(escape(cs.join("")));
    };
    /* 顺手排掉已到的回包，避免长会话撑满 TCP 缓冲。 */
    conn.drain = function () { try { while (conn.recv() !== null) {} } catch (e) {} };
    conn.close = function () { if (CURR === conn) CURR = null; try { conn.sock.close(); } catch (e) {} };
    CURR = conn;
    return conn;
}

function vtouchSend(c, line) { if (!c.watch) c.drain(); c.send(line); }
function vtouchReset() { vtouchSend(vtouchCur(), "reset"); }
/* 订阅物理触摸流：daemon 此后推送 pev 行；订阅后发包不再 drain（读线程消费回包）。 */
function vtouchSub(c) { c.drain(); c.send("sub"); c.watch = true; }
function vtouchUnsub(c) { c.watch = false; c.send("unsub"); c.drain(); }
/* pev <slot> <down|move|up> <lx> <ly> -> {slot,action,x,y}；非 pev 行返回 null。 */
function vtouchParseEv(line) {
    if (!line || line.indexOf("pev ") !== 0) return null;
    var p = line.split(" ");
    if (p.length !== 5) return null;
    var slot = +p[1], x = +p[3], y = +p[4];
    if (isNaN(slot) || isNaN(x) || isNaN(y)) return null;
    if (p[2] !== "down" && p[2] !== "move" && p[2] !== "up") return null;
    return { slot: slot, action: p[2], x: x, y: y };
}
function vtouchStop() {
    CURR = null;
    /* 面板 full 模式就是 daemon：脚本退出时一并收掉（EVIOCGRAB 随进程退出释放）。 */
    if (vtouchUiAlive()) {
        /* 脚本没有常驻逻辑（读循环 / setInterval）时，调完 uiStart 立刻就走到这里：
         * 面板起来不到 2 秒又被收掉，肉眼就是「窗口不弹出来」。这种静默失败要喊出来。 */
        if (UI_T0 && Date.now() - UI_T0 < 2000)
            toastLog("面板起来不到 2s 就被脚本退出收掉了：脚本末尾缺少常驻逻辑（读循环 / setInterval）");
        vtouchUiStop();
    }
    var dp = "/data/local/tmp/vtouch-runtime/vtouchd.pid";
    vtouchKillAll("vtouchd");
    shell("rm -f " + dp, true);
}

/* ---- 旋转坐标层：daemon 只认竖屏物理坐标（启动时 -w/-h portrait）。
 * 横屏时 device.width/height 会对调，注入前 C2P、绘制/回调用 P2C。
 * 方向走 Display.getRotation（无 shell，面板起来后也可用）。
 * R1 方向经 PJZ110 横屏真机闭环验证（tap 落点误差≤1px）；R2 为 180° 无歧义；R3 取 R1 镜像对称，待验证。 */
function vtouchRot() {
    try {
        var wm = context.getSystemService(android.content.Context.WINDOW_SERVICE);
        return wm.getDefaultDisplay().getRotation();
    } catch (e) { return 0; }
}
function vtouchPortrait() {
    var w = device.width, h = device.height;
    return { w: Math.min(w, h), h: Math.max(w, h) };
}
function vtC2P(x, y) {
    var r = vtouchRot(), P = vtouchPortrait();
    if (r === 1) return { x: P.w - 1 - y, y: x };
    if (r === 3) return { x: y, y: P.h - 1 - x };
    if (r === 2) return { x: P.w - 1 - x, y: P.h - 1 - y };
    return { x: x, y: y };
}
function vtP2C(x, y) {
    var r = vtouchRot(), P = vtouchPortrait();
    if (r === 1) return { x: y, y: P.w - 1 - x };
    if (r === 3) return { x: P.h - 1 - y, y: x };
    if (r === 2) return { x: P.w - 1 - x, y: P.h - 1 - y };
    return { x: x, y: y };
}

/* ---- 入口：仪式全包，业务只写触摸逻辑 ----
 * vt.run(function () { vt.finger().tap(540, 1200); });
 * 主线程必须直接返回（Looper 泵事件），阻塞只在业务线程。 */
function vtouchRun(fn) {
    device.wakeUpIfNeeded();
    threads.start(function () {
        var c = null;
        try {
            device.keepScreenOn(60 * 1000);
            vtouchEnsure();
            c = vtouchConnect();
            fn();
            c.close();
            device.cancelKeepingAwake();
        } catch (e) {
            toastLog("vtouch 失败: " + e);
            try { if (c) c.close(); } catch (e2) {}
        } finally {
            CURR = null;
        }
        try { vtouchStop(); } catch (e3) {}
        exit();
    });
}

/* ---- Finger 对象：自动分配空闲 slot，也支持显式 slot ---- */
function Finger(conn, slot) { this.conn = conn; this.slot = slot; this.downState = false; }
Finger.prototype.down = function (x, y) {
    var q = vtC2P(x, y);
    vtouchSend(this.conn, "down " + this.slot + " " + Math.round(q.x) + " " + Math.round(q.y));
    this.downState = true;
    return this;
};
Finger.prototype.move = function (x, y) {
    if (!this.downState) return this;
    var q = vtC2P(x, y);
    vtouchSend(this.conn, "move " + this.slot + " " + Math.round(q.x) + " " + Math.round(q.y));
    return this;
};
Finger.prototype.up = function () {
    if (!this.downState) return this;
    vtouchSend(this.conn, "up " + this.slot);
    this.downState = false;
    return this;
};
Finger.prototype.tap = function (x, y, ms) {
    this.down(x, y); sleep(ms == null ? 60 : ms); return this.up();
};
Finger.prototype.swipe = function (x1, y1, x2, y2, ms) {
    var total = ms == null ? 300 : ms, n = Math.max(2, Math.round(total / 16.7)), i;
    this.down(x1, y1);
    for (i = 1; i <= n; i++) { sleep(total / n); this.move(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n); }
    return this.up();
};
Finger.prototype.frame = function (state, x, y) {
    return vtouchFrame([{ slot: this.slot, state: state, x: x, y: y }]);
};
Finger.prototype.state = function () { return this.downState ? "down" : "up"; };
function vtouchFinger(slot) {
    var c = vtouchCur(), i;
    if (!c.fingers) c.fingers = {};
    if (slot === undefined || slot === null) {
        for (i = 0; i <= 9; i++) if (!c.fingers[i] || !c.fingers[i].downState) break;
        if (i > 9) throw new Error("无空闲 slot");
        slot = i;
    } else {
        slot = Math.round(slot);
        if (slot < 0 || slot > 9) throw new Error("slot 0~9");
    }
    if (!c.fingers[slot]) c.fingers[slot] = new Finger(c, slot);
    return c.fingers[slot];
}
function vtouchFrame(pts) {
    var c = vtouchCur(), i, p, q;
    vtouchSend(c, "begin_frame");
    for (i = 0; i < pts.length; i++) {
        p = pts[i]; q = vtC2P(p.x, p.y);
        vtouchSend(c, "point " + p.slot + " " + p.state + " " + Math.round(q.x) + " " + Math.round(q.y));
        if (c.fingers && c.fingers[p.slot]) c.fingers[p.slot].downState = p.state !== "up";
    }
    vtouchSend(c, "end_frame");
}
'''

DEMO = """/* ---- 纯库，无副作用：加载只定义函数，不执行任何动作 ---- */
module.exports = {
    run: vtouchRun,
    ensure: vtouchEnsure,
    install: vtouchInstall,
    connect: vtouchConnect,
    send: vtouchSend,
    finger: vtouchFinger,
    Finger: Finger,
    frame: vtouchFrame,
    reset: vtouchReset,
    stop: vtouchStop,
    rot: vtouchRot,
    c2p: vtC2P,
    p2c: vtP2C,
    rgPush: rgPush,
    rgList: rgList,
    createEngine: rgCreateEngine,
    rgParseEv: rgParseEv,
    sub: vtouchSub,
    unsub: vtouchUnsub,
    parseEv: vtouchParseEv,
    uiStart: vtouchUiStart,
    uiStop: vtouchUiStop,
    uiRestart: vtouchUiRestart,
    uiAlive: vtouchUiAlive,
    uiPid: vtouchUiPid,
    uiDeploy: vtouchUiDeploy,
    uiTail: vtouchUiTail,
    UI_DIR: VTOUCH_UI_DIR,
    BIN: VTOUCH_BIN,
    HOST: VTOUCH_HOST,
    PORT: VTOUCH_PORT
};
"""


UI_BOOT = r"""
/* ---- ImGui 面板启动：单文件、脚本一键控制（设备侧不需要任何 .sh） ----
 * dex + so 内嵌在本文件里：首次 uiStart() 释放到 /data/local/tmp/vtouch-ui/（root，一次），
 * 之后 md5 一致就直接复用。起来的面板就是 daemon：
 *   EVIOCGRAB 物理触摸 + WS 127.0.0.1:27183 + regions.conf；触摸合并后照常回到系统。
 *
 *   var vt = require("/sdcard/vtouch_bundle.js");
 *   vt.uiStart();          // 起面板；已在跑则复用
 *   ...业务：vt.run(...) / vt.connect() + Finger / 回触...
 *   vt.uiStop();           // 收面板并释放 grab（vt.stop() 也会连面板一起收）
 *
 * 进程真相只认 /proc：app_process 把 comm 设成 nice-name，pid 文件只是书签，不作存活依据。 */
var VTOUCH_UI_DIR = "/data/local/tmp/vtouch-ui";
var VTOUCH_UI_RUN = "/data/local/tmp/vtouch-runtime";
var VTOUCH_UI_PID = VTOUCH_UI_RUN + "/vtouch-ui.pid";
var VTOUCH_UI_LOG = VTOUCH_UI_RUN + "/vtouch-ui.log";

/* 进程真相 = pidof（app_process 的 /proc/<pid>/comm 是 "main"，不是 nice-name，
 * comm/pid 文件都不能当依据）。pidof 是单条短命令：不用循环、不用管道，AutoJS 的
 * shell(cmd,true) 处理得稳。 */
function vtouchPids(name) {
    var r = shell("pidof " + name, true);
    var str = (r && r.result) ? String(r.result).replace(/^\s+|\s+$/g, "") : "";
    return str.length ? str.split(/\s+/) : [];
}
function vtouchKillAll(name) {
    var i, p = vtouchPids(name);
    for (i = 0; i < 8 && p.length; i++) {
        shell("kill -9 " + p.join(" "), true);
        sleep(150);
        p = vtouchPids(name);
    }
    return p;
}
function vtouchUiAlive() { return vtouchPids("vtouch-ui").length > 0; }
function vtouchUiPid() { var p = vtouchPids("vtouch-ui"); return p.length ? p[0] : null; }
function vtouchUiTail(n) {
    var r = shell("tail -n " + (n || 20) + " " + VTOUCH_UI_LOG, true);
    return (r && r.result) ? String(r.result) : "";
}
/* 释放内嵌二进制：md5 与设备一致就跳过，别每次启动都写 1MB */
function vtouchUiDeployed() {
    var r = shell("md5sum " + VTOUCH_UI_DIR + "/classes.dex " + VTOUCH_UI_DIR + "/libtestimgui.so", true);
    var str = (r && r.result) ? String(r.result) : "";
    return str.indexOf(VTOUCH_UI_DEX_MD5) >= 0 && str.indexOf(VTOUCH_UI_SO_MD5) >= 0;
}
function vtouchUiStage(bytes, name) {
    var f = context.getFilesDir().getAbsolutePath() + "/" + name;
    var fos = new java.io.FileOutputStream(f);
    try { fos.write(bytes); } finally { try { fos.close(); } catch (e) {} }
    return f;
}
function vtouchUiGunzip(b64) {
    var gz = android.util.Base64.decode(b64, android.util.Base64.DEFAULT);
    var ins = new java.util.zip.GZIPInputStream(new java.io.ByteArrayInputStream(gz));
    var out = new java.io.ByteArrayOutputStream(VTOUCH_UI_SO_SIZE);
    var buf = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, 65536);
    var n; while ((n = ins.read(buf)) > 0) out.write(buf, 0, n);
    ins.close();
    return out.toByteArray();
}
function vtouchUiDeploy() {
    if (vtouchUiDeployed()) return false;
    log("[vtouch] 释放面板 dex=" + VTOUCH_UI_DEX_SIZE + "B so=" + VTOUCH_UI_SO_SIZE + "B（内嵌 gz " + VTOUCH_UI_SO_GZ_SIZE + "B）");
    var dex = vtouchUiStage(android.util.Base64.decode(VTOUCH_UI_DEX_B64, android.util.Base64.DEFAULT), "vtouch-ui.dex.stage");
    var so = vtouchUiStage(vtouchUiGunzip(VTOUCH_UI_SO_GZ_B64), "vtouch-ui.so.stage");
    var r = shell("mkdir -p " + VTOUCH_UI_DIR
        + " && cp -f '" + dex + "' " + VTOUCH_UI_DIR + "/classes.dex"
        + " && cp -f '" + so + "' " + VTOUCH_UI_DIR + "/libtestimgui.so"
        + " && chmod 644 " + VTOUCH_UI_DIR + "/classes.dex " + VTOUCH_UI_DIR + "/libtestimgui.so", true);
    if (!r || r.code !== 0) throw new Error("面板释放失败: " + ((r && (r.error || r.result)) || "unknown"));
    if (!vtouchUiDeployed()) throw new Error("面板释放后 md5 不符，检查 /data/local/tmp/vtouch-ui/");
    log("[vtouch] 面板二进制就绪");
    return true;
}
/* 启动：直接在当前 root shell 里后台起（套 sh -c 反而会被 su 收走子进程，别改） */
var UI_T0 = 0;   /* 本次脚本里面板起来的时刻（只用于「刚起就被脚本退出收掉」这条诊断） */
function vtouchUiStart() {
    if (vtouchUiAlive()) return true;
    var left = vtouchKillAll("vtouch-ui");
    if (left.length) throw new Error("旧面板清不掉 pid=" + left.join(","));
    vtouchUiDeploy();
    var launch = "U=" + VTOUCH_UI_DIR + ";R=" + VTOUCH_UI_RUN + ";mkdir -p $R;"
        + "S=$(wm size);S=${S##*Physical size: };W=${S%%x*};H=${S##*x};if [ $W -gt $H ];then T=$W;W=$H;H=$T;fi;"
        + "CLASSPATH=$U/classes.dex nohup app_process /system/bin --nice-name=vtouch-ui VTouchUI $W $H"
        + " >$R/vtouch-ui.log 2>&1 </dev/null & echo $!>$R/vtouch-ui.pid";
    shell(launch, true);
    var i;
    for (i = 0; i < 24 && !vtouchUiAlive(); i++) sleep(150);
    if (!vtouchUiAlive()) throw new Error("面板没起来: " + vtouchUiTail(8));
    UI_T0 = Date.now();
    return true;
}
function vtouchUiStop() {
    vtouchKillAll("vtouch-ui");
    shell("rm -f " + VTOUCH_UI_PID, true);
    return !vtouchUiAlive();
}
function vtouchUiRestart() { vtouchUiStop(); return vtouchUiStart(); }
"""


def write_out(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")
    r = subprocess.run(["node", "--check", str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr.strip())
        return False
    return True


ONE_LIB = r'''
/* ---- 区域：唯一归属是面板（面板读写 /data/local/tmp/vtouch-runtime/regions.conf），
 * 脚本侧不落任何库：只「回读」面板当前表 / 「下发」自己的表。这样不存在第二份状态，
 * 也不会把旧版本存下来的区域再捞回来。
 * 区域格式: {id, name, x1, y1, x2, y2, enabled}（矩形）/{id, name, type:"circle", cx, cy, r}，
 * 一律竖屏物理坐标（daemon 坐标系）；engine.feed 吃的也是竖屏坐标（pev 原样）。
 * 名称只活在脚本侧（线协议只有 id + 几何），所以 rgList 回的 name 就是 id。 */
function rgInRect(r, x, y) { return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2; }
function rgHit(r, x, y) {
    if (r.type === "circle") { var dx = x - r.cx, dy = y - r.cy; return dx * dx + dy * dy <= r.r * r.r; }
    return rgInRect(r, x, y);
}
/* 下发：region clear + 逐条 add；面板收到即生效并落盘（重启还在）。c = vt.connect() 的连接。 */
function rgPush(c, rs) {
    var i, r, en;
    if (!c || !c.send) return false;
    try {
        c.send("region clear");
        for (i = 0; i < rs.length; i++) {
            r = rs[i]; en = (r.enabled === false ? 0 : 1);
            if (r.type === "circle")
                c.send("region add " + r.id + " 1 " + Math.round(r.cx) + " " + Math.round(r.cy) + " " + Math.round(r.r) + " 0 " + en);
            else
                c.send("region add " + r.id + " 0 " + Math.round(r.x1) + " " + Math.round(r.y1) + " " + Math.round(r.x2) + " " + Math.round(r.y2) + " " + en);
        }
        return true;
    } catch (e) { return false; }
}
/* 回读面板当前表；必须在开读包循环之前调（会消费回包行），超时/无连接返回 []。 */
function rgList(c) {
    var out = [], dl, i, p, lines, s, id, t;
    if (!c || !c.send) return out;
    try {
        c.drain();   /* 先把队列里 rgPush/clear/add 的 ok 回包吃掉，否则 region 行还没到就超时 */
        c.send("region list");
        dl = Date.now() + 600;
        while (Date.now() < dl) {
            s = c.recv();
            if (!s) { sleep(10); continue; }
            lines = ("" + s).split("\n");
            for (i = 0; i < lines.length; i++) {
                p = lines[i].replace(/^\s+|\s+$/g, "").split(" ");
                if (p[0] === "region" && p.length >= 8) {
                    id = p[1]; t = +p[2];
                    if (t === 1) out.push({ id: id, name: id, type: "circle", cx: +p[3], cy: +p[4], r: +p[5], enabled: p[7] !== "0" });
                    else out.push({ id: id, name: id, x1: +p[3], y1: +p[4], x2: +p[5], y2: +p[6], enabled: p[7] !== "0" });
                } else if (p[0] === "end" || (p[0] === "ok" && p[1] === "0")) {
                    return out;
                }
            }
        }
    } catch (e) {}
    return out;
}
/* daemon 原生区域事件：region_ev <id> <down|up|enter|exit|move> <slot> <lx> <ly>
 * -> {id, ev, slot, x, y}；不是 region_ev 行返回 null。
 * 用它可以「只按 id 分发」，不必在脚本里再存一份区域表、再算一遍命中。
 * 注意：只报物理手指（daemon 只扫物理槽），虚拟触摸不产生事件。 */
function rgParseEv(line) {
    var p;
    if (!line || line.indexOf("region_ev ") !== 0) return null;
    p = line.replace(/^\s+|\s+$/g, "").split(" ");
    if (p.length < 6) return null;
    return { id: p[1], ev: p[2], slot: +p[3], x: +p[4], y: +p[5] };
}
function rgCreateEngine(regions, handlers) {
    var inside = {}, fingers = {};
    return {
        fingers: function () { var o = [], k; for (k in fingers) o.push(fingers[k]); return o; },
        setRegions: function (rs) { regions = rs; },
        feed: function (p) {
            var evts = [], i, r, f = fingers[p.slot] || (fingers[p.slot] = { slot: p.slot, x: 0, y: 0, down: false });
            f.x = p.x; f.y = p.y;
            if (p.action === "down") f.down = true;
            if (p.action === "up") f.down = false;
            if (!inside[p.slot]) inside[p.slot] = {};
            for (i = 0; i < regions.length; i++) {
                r = regions[i];
                if (r.enabled === false) continue;
                var hit = rgHit(r, p.x, p.y), was = !!inside[p.slot][r.id];
                if (p.action === "down" && hit) { inside[p.slot][r.id] = true; evts.push({ type: "down", region: r, finger: { slot: f.slot, x: f.x, y: f.y } }); }
                else if (p.action === "up" && (hit || was)) { delete inside[p.slot][r.id]; evts.push({ type: "up", region: r, finger: { slot: f.slot, x: f.x, y: f.y } }); }
                else if (p.action === "move" && hit) { inside[p.slot][r.id] = true; evts.push({ type: "move", region: r, finger: { slot: f.slot, x: f.x, y: f.y } }); if (!was) evts.push({ type: "enter", region: r, finger: { slot: f.slot, x: f.x, y: f.y } }); }
                else if (hit && !was) { inside[p.slot][r.id] = true; evts.push({ type: "enter", region: r, finger: { slot: f.slot, x: f.x, y: f.y } }); }
                else if (!hit && was) { delete inside[p.slot][r.id]; evts.push({ type: "exit", region: r, finger: { slot: f.slot, x: f.x, y: f.y } }); }
            }
            if (handlers) for (i = 0; i < evts.length; i++) {
                var e = evts[i], h = handlers["on" + e.type[0].toUpperCase() + e.type.slice(1)];
                if (h) h(e.region, e.finger);
            }
            if (p.action === "up") delete fingers[p.slot];
            return evts;
        }
    };
}
'''

def _b64_lines(b64, n=76):
    """分行拼接：单行超长 base64 会被中间设备/WAF 拦（403），分行后单行短。"""
    return '"\n    + "'.join(b64[i:i + n] for i in range(0, len(b64), n))


def main() -> int:
    if "--check" in sys.argv:
        r = subprocess.run(["node", "--check", str(OUT)], capture_output=True, text=True)
        print("bundle check:", "OK" if r.returncode == 0 else r.stderr.strip())
        return 0 if r.returncode == 0 else 1

    raw = BIN.read_bytes()
    b64 = base64.b64encode(raw).decode("ascii")
    # B64 分行拼接：单行 30KB 连续 base64 会被内网 HIS WAF 拦（403）；分行后单行短
    _b64js = _b64_lines(b64)
    blob = (
        "/* ---- 内嵌二进制（构建机填入） ---- */\n"
        "var VTOUCH_BIN_SIZE = %d;\n"
        'var VTOUCH_BIN_B64 = "%s";\n' % (len(raw), _b64js)
    )
    for f in (UI_SO, UI_DEX):
        if not f.exists():
            print("缺 %s — 先跑: sh scripts/build_ui.sh" % f)
            return 1
    so_raw = UI_SO.read_bytes()
    so_gz = gzip.compress(so_raw, 9)
    dex_raw = UI_DEX.read_bytes()
    uiblob = (
        "/* ---- 内嵌面板（构建机填入）：dex 原样 b64；so 走 gz+b64（%dKB → %dKB） ---- */\n"
        % (len(so_raw) // 1024, len(so_gz) // 1024)
        + "var VTOUCH_UI_DEX_SIZE = %d;\n" % len(dex_raw)
        + 'var VTOUCH_UI_DEX_B64 = "%s";\n' % _b64_lines(base64.b64encode(dex_raw).decode("ascii"))
        + "var VTOUCH_UI_SO_SIZE = %d;\n" % len(so_raw)
        + "var VTOUCH_UI_SO_GZ_SIZE = %d;\n" % len(so_gz)
        + 'var VTOUCH_UI_SO_GZ_B64 = "%s";\n' % _b64_lines(base64.b64encode(so_gz).decode("ascii"))
        + 'var VTOUCH_UI_DEX_MD5 = "%s";\n' % hashlib.md5(dex_raw).hexdigest()
        + 'var VTOUCH_UI_SO_MD5 = "%s";\n' % hashlib.md5(so_raw).hexdigest()
    )
    if not write_out(OUT, CORE + "\n" + blob + "\n" + uiblob + "\n" + ONE_LIB + "\n" + UI_BOOT + "\n" + DEMO):
        return 1
    print("bundle: %s (%d bytes)" % (OUT, OUT.stat().st_size))
    print("panel in bundle: dex=%dB so=%dB(gz %dB) md5 dex=%s so=%s"
          % (len(dex_raw), len(so_raw), len(so_gz), hashlib.md5(dex_raw).hexdigest()[:8], hashlib.md5(so_raw).hexdigest()[:8]))
    print("bundle syntax OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
