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
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "vtouchd"
OUT = ROOT / "clients" / "vtouch_bundle.js"

CORE = '''/* ============================================================================
 * VTouch 单文件包：二进制内嵌 + 一层薄函数。由 scripts/build_bundle.py 生成，勿手改。
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
 * daemon 坐标系恒为竖屏物理（wm size portrait）：W/H 归一化 min/max，横屏开机也不错位。
 * 免 su 直启：先做纯 TCP 探测（127.0.0.1:27183 通 = vtouchd 已在运行，如 AVD 上 adb root 直启），
 * 通则完全不碰 su——su 通道不可用时（守护未起/无 root）也能直连已运行的 vtouchd。 */
function vtouchEnsure() {
    try {
        var __s = new java.net.Socket();
        __s.connect(new java.net.InetSocketAddress("127.0.0.1", VTOUCH_PORT), 200);
        __s.close();
        return;
    } catch (e) {}
    var r = shell("B=/data/local/tmp/vtouch-runtime;D=" + VTOUCH_BIN + ";"
        + "[ -f $B/vtouchd.pid ]&&kill -0 $(cat $B/vtouchd.pid 2>/dev/null) 2>/dev/null&&exit 0;"
        + "[ -x $D ]||exit 11;"
        + "mkdir -p $B;killall vtouchd 2>/dev/null;rm -f $B/vtouchd.pid;"
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
        + "mkdir -p $B;killall vtouchd 2>/dev/null;rm -f $B/vtouchd.pid;"
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
    shell("killall vtouchd 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/vtouchd.pid", true);
}

/* ---- 旋转坐标层：daemon 只认竖屏物理坐标（启动时 -w/-h portrait）。
 * 横屏时 device.width/height 会对调，注入前 C2P、绘制/回调用 P2C。
 * 方向走 Display.getRotation（无 shell，floaty 建出后也可用）。
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
    loadRegions: rgLoad,
    rgSave: rgSave,
    createEngine: rgCreateEngine,
    sub: vtouchSub,
    unsub: vtouchUnsub,
    parseEv: vtouchParseEv,
    uiSource: VTOUCH_UI_SRC,
    BIN: VTOUCH_BIN,
    HOST: VTOUCH_HOST,
    PORT: VTOUCH_PORT
};
"""


def write_out(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")
    r = subprocess.run(["node", "--check", str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr.strip())
        return False
    return True


ONE_LIB = '''
/* ---- 区域监听（store + engine + overlay，单文件内联） ----
 * 区域格式: {id, name, x1, y1, x2, y2, enabled}（矩形）/{id, name, type:"circle", cx, cy, r}，
 * 一律存竖屏物理坐标（daemon 坐标系）；横屏时绘制/回调由 P2C 换算。存 storages "vtouch_regions"。
 * UI 框选页/管理页读写同一份，无需改这里。JS 内 engine 的 feed 吃竖屏坐标（pev 原样）。 */
var _rgStore = storages.create("vtouch_regions");
function rgLoad() {
    try {
        var rs = _rgStore.get("regions");
        if (rs && rs.length) return rs;
    } catch (e) {}
    return [{ id: "btn", name: "按钮区", x1: 400, y1: 1000, x2: 1040, y2: 1400, enabled: true }];
}
function rgInRect(r, x, y) { return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2; }
function rgHit(r, x, y) {
    if (r.type === "circle") { var dx = x - r.cx, dy = y - r.cy; return dx * dx + dy * dy <= r.r * r.r; }
    return rgInRect(r, x, y); /* 老数据无 type，按矩形 */
}
function rgSave(rs) { try { _rgStore.put("regions", rs); } catch (e) {} }
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

UI_SRC = '''/* 区域 overlay + 管理 UI + watcher 启动器：调用侧 eval(vt.uiSource) 进主上下文执行。
 * 原因：Java bridge 回调（线程/事件/点击/画布）不能定义在 require 模块里。
 * 只用 vt.loadRegions / vt.rgSave / vt.ensure / vt.connect / vt.sub / vt.createEngine / vt.parseEv / vt.finger / vt.stop。
 * 调用侧只剩：eval + bootWatch(handlers) + 业务。管理：ui() 开 / uiClose() 关。
 * 注意：本源码 eval 进调用者主上下文，只能用 vt 导出的函数；裸内部函数不可见，
 * 旋转换算走别名 vtC2P/vtP2C/vtouchRot（= vt.c2p/vt.p2c/vt.rot）。 */
var vtC2P = vt.c2p, vtP2C = vt.p2c, vtouchRot = vt.rot;
var g_ovW = null, g_ovR = [], g_ovF = [], g_ovH = {}, g_ovLoc = null;
var g_ovRot = -1, g_ovWdt = 0, g_ovHgt = 0;
function ovShow(rs) {
    g_ovR = rs;
    if (g_ovW) return;
    g_ovW = floaty.rawWindow('<frame><canvas id="board" layout_weight="1"/></frame>');
    g_ovW.setSize(device.width, device.height);
    g_ovW.setTouchable(false);
    try { g_ovRot = vtouchRot(); } catch (e) { g_ovRot = 0; }
    g_ovWdt = device.width; g_ovHgt = device.height;
    g_ovW.board.on("draw", function (canvas) {
        var i, r, p, q, qa, qb, lx, ly;
        /* 线程池 draw（AutoJs6 6.x ScriptCanvasView mDrawingThreadPool）与 ovUpdate/ovSet/ovClose 并发：
         * 先快照全局引用，防执行中途被置 null/换数组。区域存竖屏坐标，这里 P2C 到当前屏绘制。 */
        var w = g_ovW, rs = g_ovR, fs = g_ovF;
        if (!w) return;
        try { canvas.drawColor(colors.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR); } catch (e) {}
        var oy = 0, ox = 0;
        try {
            if (!g_ovLoc) g_ovLoc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
            w.board.getLocationOnScreen(g_ovLoc);
            ox = g_ovLoc[0]; oy = g_ovLoc[1];
        } catch (e) {}
        for (i = 0; i < rs.length; i++) {
            r = rs[i];
            if (r.hidden) continue;
            p = new Paint(); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(3); p.setColor(colors.RED);
            if (g_ovH[r.id] && Date.now() - g_ovH[r.id] < 400) { p.setStrokeWidth(6); p.setColor(colors.GREEN); }
            if (r.type === "circle") { q = vtP2C(r.cx, r.cy); canvas.drawCircle(q.x - ox, q.y - oy, r.r, p); lx = q.x - r.r; ly = q.y - r.r; }
            else { qa = vtP2C(r.x1, r.y1); qb = vtP2C(r.x2, r.y2); lx = Math.min(qa.x, qb.x); ly = Math.min(qa.y, qb.y); canvas.drawRect(lx - ox, ly - oy, Math.max(qa.x, qb.x) - ox, Math.max(qa.y, qb.y) - oy, p); }
            p = new Paint(); p.setColor(colors.WHITE); p.setTextSize(36);
            canvas.drawText(r.name || r.id, lx - ox + 8, ly - oy + 40, p);
        }
        for (i = 0; i < fs.length; i++) {
            var f = fs[i];
            if (!f.down) continue;
            q = vtP2C(f.x, f.y);
            p = new Paint(); p.setColor(colors.BLUE);
            canvas.drawCircle(q.x - ox, q.y - oy, 40, p);
            canvas.drawText("s" + f.slot, q.x - ox + 44, q.y - oy, p);
        }
    });
}
/* 管理窗/小球建在 overlay 之后会盖住区域：重建 overlay 回顶到最上（区域/手指状态保留）。 */
function ovRetop() {
    if (!g_ovW) return;
    var rs = g_ovR, fs = g_ovF;
    try { g_ovW.close(); } catch (e) {}
    g_ovW = null;
    ovShow(rs); g_ovF = fs;
    try { g_ovW.board.postInvalidate(); } catch (e2) {}
}
/* 旋转后 overlay 尺寸过期：重设尺寸（映射每帧实时读方向，不用动别的）。 */
function ovSync() {
    var r = 0, w = device.width, h = device.height;
    try { r = vtouchRot(); } catch (e) {}
    if (g_ovW && (r !== g_ovRot || w !== g_ovWdt || h !== g_ovHgt)) {
        g_ovRot = r; g_ovWdt = w; g_ovHgt = h;
        try { g_ovW.setSize(w, h); } catch (e2) {}
        try { g_ovW.board.postInvalidate(); } catch (e3) {}
    } else if (!g_ovW) { g_ovRot = r; g_ovWdt = w; g_ovHgt = h; }
}
/* 统一旋转同步（2 秒轮询里调）：overlay 重建回顶 + 管理窗改尺寸/拉回屏内 + 框选关闭。
 * overlay 重建比重设尺寸更彻底（尺寸+层级一次到位）；管理窗只调尺寸位置，列表状态不动。 */
function rotSync() {
    var r = 0, w = device.width, h = device.height, hadCap = false, first = false;
    try { r = vtouchRot(); } catch (e) {}
    /* 一致性门：监听回调有时跑在 metrics 刷新前（方向已变、尺寸还是旧的），
     * 此时刷一遍等于没刷。方向奇偶必须和宽高对调一致，否则等一拍再看。 */
    if ((r % 2 !== 0) !== (w > h)) {
        try { setTimeout(function () { try { rotSync(); } catch (e2) {} }, 500); } catch (e3) {}
        return;
    }
    first = (g_uiRot === -1);
    if (!first && r === g_uiRot && w === g_uiWdt && h === g_uiHgt) return;
    g_uiRot = r; g_uiWdt = w; g_uiHgt = h;
    hadCap = !!g_capW;
    try {
        uiRun(function () {
            /* 首 sync 若 overlay 刚建好且尺寸已对（ovShow 缓存一致），跳过重建防闪屏。 */
            var ovMatch = false;
            try { ovMatch = !!g_ovW && r === g_ovRot && w === g_ovWdt && h === g_ovHgt; } catch (e0) {}
            if (g_ovW && !ovMatch) {
                var rs = g_ovR, fs = g_ovF;
                try { g_ovW.close(); } catch (e2) {}
                g_ovW = null;
                ovShow(rs); g_ovF = fs;
                try { g_ovW.board.postInvalidate(); } catch (e3) {}
            }
            if (g_uiW) {
                try { var __s2 = uiSize(); g_uiW.setSize(__s2.w, __s2.h); } catch (e4) {}
                try {
                    var x = g_uiW.getX(), y = g_uiW.getY();
                    g_uiW.setPosition(Math.max(0, Math.min(x, w - 200)), Math.max(0, Math.min(y, h - 200)));
                } catch (e5) {}
            }
            if (g_capW) capClose();
        });
    } catch (e6) { try { log("rotSync FAIL " + e6); } catch (e7) {} }
    if (!first) {
        try { toast("已旋转自适应"); } catch (e8) {}
        if (hadCap) { try { toast("框选中断，请重框"); } catch (e9) {} }
    }
    /* 确认复核：门只保证方向尺寸一致，900ms 后再看一眼，metrics 若还有滞后自动纠正（无变化则早退零开销）。 */
    try { setTimeout(function () { try { rotSync(); } catch (e10) {} }, 900); } catch (e11) {}
}
function ovUpdate(f) { g_ovF = f || []; try { g_ovW.board.postInvalidate(); } catch (e) {} }
function ovFlash(id) { g_ovH[id] = Date.now(); }
function ovSet(rs) { g_ovR = rs; }
function ovClose() { try { if (g_ovW) g_ovW.close(); } catch (e) {} g_ovW = null; }
function ovPreview(on) { if (on) ovShow(vt.loadRegions()); else ovClose(); }
/* ---- 管理 UI（深色卡片）：列表查看 | 开关触发 | 显隐预览 | 删除 | ＋矩形/圆形框选 | 预览总开关
 * 标题栏按住拖动；最小化成小悬浮球（点球恢复，可拖）。
 * 窗体固定尺寸 + scroll 权重，多行真滚动；打开后 ovRetop() 把区域层顶回最上。 */
var g_uiW = null, g_capW = null, g_cap = null, g_minW = null;
var g_uiRot = -1, g_uiWdt = 0, g_uiHgt = 0;
var g_uiH = new android.os.Handler(android.os.Looper.getMainLooper());
function rgInfo(r) {
    var head = (r.enabled === false ? "[关] " : "[开] ") + (r.hidden ? "[隐] " : "") + (r.name || r.id);
    if (r.type === "circle") return head + " | O r=" + r.r + " @ " + r.cx + "," + r.cy;
    return head + " | 口 " + (r.x2 - r.x1) + "x" + (r.y2 - r.y1) + " @ " + r.x1 + "," + r.y1;
}
/* 八槽静态行：XML 里写死，不用程序化 view（API36 ColorOS 上程序化 Button 默认色会毒崩 TextView 绘制）。 */
function uiRowAct(k, what) {
    var rs = vt.loadRegions();
    if (k < 0 || k >= rs.length) return;
    var r = rs[k], i;
    if (what === "t") r.enabled = (r.enabled === false);
    else if (what === "v") r.hidden = !r.hidden;
    else if (what === "d") { var all = []; for (i = 0; i < rs.length; i++) if (i !== k) all.push(rs[i]); rs = all; }
    vt.rgSave(rs); ovSet(rs); uiRefresh();
    toast(what === "d" ? "已删除" : "已更新 " + (r.name || r.id));
}
function uiRefresh() {
    if (!g_uiW) return;
    try {
        g_uiH.post(new JavaAdapter(java.lang.Runnable, { run: function () {
            try { uiRefreshDo(); } catch (e) { log("uiRefresh FAIL " + e); }
        } }));
    } catch (e) { log("uiRefresh post FAIL " + e); }
}
function uiRefreshDo() {
    var rs = vt.loadRegions();
    var extra = rs.length > 8 ? "（仅显示前8个）" : "";
    g_uiW.title.setText("区域管理 (" + rs.length + "个)" + extra);
    g_uiW.preview.setText(g_ovW ? "预览：开" : "预览：关");
    if (rs.length > 0) { g_uiW.s0t.setText(rgName(rs[0])); g_uiW.s0m.setText(rgMeta(rs[0])); g_uiW.s0d.setText(rs[0].enabled === false ? "○" : "●"); g_uiW.s0.setVisibility(0); }
    else { g_uiW.s0.setVisibility(8); }
    if (rs.length > 1) { g_uiW.s1t.setText(rgName(rs[1])); g_uiW.s1m.setText(rgMeta(rs[1])); g_uiW.s1d.setText(rs[1].enabled === false ? "○" : "●"); g_uiW.s1.setVisibility(0); }
    else { g_uiW.s1.setVisibility(8); }
    if (rs.length > 2) { g_uiW.s2t.setText(rgName(rs[2])); g_uiW.s2m.setText(rgMeta(rs[2])); g_uiW.s2d.setText(rs[2].enabled === false ? "○" : "●"); g_uiW.s2.setVisibility(0); }
    else { g_uiW.s2.setVisibility(8); }
    if (rs.length > 3) { g_uiW.s3t.setText(rgName(rs[3])); g_uiW.s3m.setText(rgMeta(rs[3])); g_uiW.s3d.setText(rs[3].enabled === false ? "○" : "●"); g_uiW.s3.setVisibility(0); }
    else { g_uiW.s3.setVisibility(8); }
    if (rs.length > 4) { g_uiW.s4t.setText(rgName(rs[4])); g_uiW.s4m.setText(rgMeta(rs[4])); g_uiW.s4d.setText(rs[4].enabled === false ? "○" : "●"); g_uiW.s4.setVisibility(0); }
    else { g_uiW.s4.setVisibility(8); }
    if (rs.length > 5) { g_uiW.s5t.setText(rgName(rs[5])); g_uiW.s5m.setText(rgMeta(rs[5])); g_uiW.s5d.setText(rs[5].enabled === false ? "○" : "●"); g_uiW.s5.setVisibility(0); }
    else { g_uiW.s5.setVisibility(8); }
    if (rs.length > 6) { g_uiW.s6t.setText(rgName(rs[6])); g_uiW.s6m.setText(rgMeta(rs[6])); g_uiW.s6d.setText(rs[6].enabled === false ? "○" : "●"); g_uiW.s6.setVisibility(0); }
    else { g_uiW.s6.setVisibility(8); }
    if (rs.length > 7) { g_uiW.s7t.setText(rgName(rs[7])); g_uiW.s7m.setText(rgMeta(rs[7])); g_uiW.s7d.setText(rs[7].enabled === false ? "○" : "●"); g_uiW.s7.setVisibility(0); }
    else { g_uiW.s7.setVisibility(8); }
}
function uiWire(v, fn) {
    v.setOnClickListener(new JavaAdapter(android.view.View.OnClickListener, { onClick: function () { try { fn(); } catch (e) {} } }));
}
/* 脚本线程调 view 写操作（收起/开关等）必须过主 looper，否则 CalledFromWrongThread。
 * 等前先清中断旗：AutoJs6 运行时偶发残留中断（sleep/广播后），否则 await 进去就抛 InterruptedException。 */
function uiOnMain(fn) {
    var latch = new java.util.concurrent.CountDownLatch(1), err = null;
    try { java.lang.Thread.interrupted(); } catch (e0) {}
    g_uiH.post(new JavaAdapter(java.lang.Runnable, { run: function () {
        try { fn(); } catch (e) { err = e; }
        latch.countDown();
    } }));
    var ok = false;
    try { ok = latch.await(3, java.util.concurrent.TimeUnit.SECONDS); } catch (e2) { throw new Error("ui 等待被中断: " + e2); }
    if (!ok) throw new Error("ui 主线程无响应");
    if (err) throw err;
}
/* 显示监听回调跑在主线程，直接调 uiOnMain 会自锁；同线程直跑，非主线程才过 latch。 */
function uiRun(fn) {
    var same = false;
    try { same = android.os.Looper.myLooper() === android.os.Looper.getMainLooper(); } catch (e) {}
    if (same) fn(); else uiOnMain(fn);
}
/* 低开销旋转监听：只在显示变化时回调（零轮询），触发 rotSync（内部自带无变化早退，天然防抖）。 */
var g_dispL = null, g_dispM = null;
function rotWatch() {
    if (g_dispL) return;
    try {
        g_dispM = context.getSystemService(android.content.Context.DISPLAY_SERVICE);
        g_dispL = new JavaAdapter(android.hardware.display.DisplayManager.DisplayListener, {
            onDisplayAdded: function (id) {},
            onDisplayRemoved: function (id) {},
            onDisplayChanged: function (id) { try { if (id === 0) rotSync(); } catch (e) {} }
        });
        g_dispM.registerDisplayListener(g_dispL, g_uiH);
    } catch (e2) { try { log("rotWatch FAIL " + e2); } catch (e3) {} g_dispL = null; g_dispM = null; }
}
function rotUnwatch() {
    try { if (g_dispM && g_dispL) g_dispM.unregisterDisplayListener(g_dispL); } catch (e) {}
    g_dispL = null; g_dispM = null;
}
function uiDragBar() {
    var lx = 0, ly = 0;
    try {
        g_uiW.titlebar.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, { onTouch: function (v, ev) {
            try {
                var a = ev.getAction();
                if (a === 0) { lx = ev.getRawX(); ly = ev.getRawY(); }
                else if (a === 2) {
                    var dx = ev.getRawX() - lx, dy = ev.getRawY() - ly;
                    lx = ev.getRawX(); ly = ev.getRawY();
                    try { g_uiW.setPosition(g_uiW.getX() + Math.round(dx), g_uiW.getY() + Math.round(dy)); } catch (e) {}
                }
            } catch (e2) {}
            return true;
        } }));
    } catch (e3) {}
}
function uiMinClose() { try { if (g_minW) g_minW.close(); } catch (e) {} g_minW = null; }
/* 最小化：关管理窗，留小悬浮球（可拖，点球恢复管理窗）。overlay 保持不动。 */
function uiMin() {
    uiClose();
    if (g_minW) return;
    g_minW = floaty.window('<button id="ball" text="管理" textSize="15sp" textColor="#FFFFFF" backgroundTint="#2563EB"/>');
    try { g_minW.setSize(208, 208); } catch (e) {}
    var sx = 0, sy = 0, wx = 0, wy = 0, moved = false;
    try {
        g_minW.ball.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, { onTouch: function (v, ev) {
            try {
                var a = ev.getAction();
                if (a === 0) { sx = ev.getRawX(); sy = ev.getRawY(); wx = g_minW.getX(); wy = g_minW.getY(); moved = false; }
                else if (a === 2) {
                    if (Math.abs(ev.getRawX() - sx) + Math.abs(ev.getRawY() - sy) > 12) moved = true;
                    if (moved) { try { g_minW.setPosition(Math.round(wx + ev.getRawX() - sx), Math.round(wy + ev.getRawY() - sy)); } catch (e2) {} }
                }
                else if (a === 1 && !moved) { try { uiMinClose(); ui(); } catch (e3) {} }
            } catch (e4) {}
            return true;
        } }));
    } catch (e5) {}
}
/* 管理窗尺寸：宽按屏 88% 但封顶（横屏不再满幅）；高竖屏 62%、横屏 92%。 */
function uiSize() {
    var w = device.width, h = device.height;
    return { w: Math.round(Math.min(w * 0.88, 1250)), h: Math.round(h * (w > h ? 0.92 : 0.62)) };
}
function rgName(r) { return (r.enabled === false ? "[关] " : "") + (r.name || r.id); }
function rgMeta(r) {
    var s = (r.hidden ? "[隐] " : "");
    if (r.type === "circle") return s + "圆 r=" + r.r + " @ " + r.cx + "," + r.cy;
    return s + "方 " + (r.x2 - r.x1) + "x" + (r.y2 - r.y1) + " @ " + r.x1 + "," + r.y1;
}
function uiRowXml(k) {
    /* 尺寸一律显式 dp：光写数字按 px 算，在高 dpi 屏上会被压扁截断；按钮用 padding 自适应宽，永不截断。 */
    return '<vertical id="s' + k + '" padding="12dp 10dp 12dp 10dp">'
        + '<horizontal gravity="center_vertical">'
        + '<text id="s' + k + 'd" text="●" textSize="12sp" textColor="#16A34A"/>'
        + '<text id="s' + k + 't" layout_weight="1" textSize="15sp" textStyle="bold" textColor="#111827" maxLines="1" ellipsize="end" marginLeft="6dp" text=""/>'
        + '</horizontal>'
        + '<text id="s' + k + 'm" textSize="12sp" textColor="#6B7280" maxLines="1" ellipsize="end" marginTop="2dp" text=""/>'
        + '<horizontal marginTop="8dp">'
        + '<button id="s' + k + 'a" layout_weight="1" text="开关" textSize="12sp" textColor="#374151" padding="0dp 8dp 0dp 8dp"/>'
        + '<button id="s' + k + 'b" layout_weight="1" text="显隐" textSize="12sp" textColor="#374151" padding="0dp 8dp 0dp 8dp" marginLeft="6dp"/>'
        + '<button id="s' + k + 'c" layout_weight="1" text="删除" textSize="12sp" textColor="#DC2626" padding="0dp 8dp 0dp 8dp" marginLeft="6dp"/>'
        + '</horizontal></vertical>'
        + '<view bg="#E5E7EB" h="1dp" marginLeft="12dp" marginRight="12dp"/>';
}
function ui() {
    if (g_uiW) { uiClose(); return; }
    uiMinClose();
    g_uiRot = -1;
    g_uiW = floaty.window(
        '<vertical padding="10dp" bg="#00000000">'
        + '<card cardCornerRadius="12dp" cardBackgroundColor="#FFFFFF" cardElevation="4dp" w="*" h="*">'
        + '<vertical padding="16dp 14dp 16dp 14dp" w="*" h="*">'
        + '<horizontal id="titlebar" gravity="center_vertical"><text id="title" layout_weight="1" textSize="18sp" textStyle="bold" textColor="#111827" text="区域管理"/>'
        + '<text id="btnMin" text="–" gravity="center" textSize="20sp" textColor="#6B7280" padding="16dp 12dp 16dp 12dp"/>'
        + '<text id="btnQuitTop" text="×" gravity="center" textSize="22sp" textColor="#6B7280" padding="16dp 12dp 16dp 12dp"/></horizontal>'
        + '<text textSize="12sp" textColor="#6B7280" maxLines="1" ellipsize="end" marginTop="2dp" text="开/关=是否触发 · 显/隐=预览是否绘制"/>'
        + '<scroll layout_weight="1" marginTop="8dp"><vertical id="rows">'
        + uiRowXml(0) + uiRowXml(1) + uiRowXml(2) + uiRowXml(3) + uiRowXml(4) + uiRowXml(5) + uiRowXml(6) + uiRowXml(7)
        + '</vertical></scroll>'
        + '<horizontal marginTop="10dp"><button id="addRect" layout_weight="1" text="＋矩形" textSize="14sp" textColor="#FFFFFF" backgroundTint="#2563EB" padding="0dp 12dp 0dp 12dp"/><button id="addCircle" layout_weight="1" text="＋圆形" textSize="14sp" textColor="#FFFFFF" backgroundTint="#2563EB" padding="0dp 12dp 0dp 12dp" marginLeft="10dp"/></horizontal>'
        + '<button id="preview" text="预览：关" textSize="14sp" marginTop="10dp" w="*" padding="0dp 12dp 0dp 12dp"/>'
        + '</vertical></card></vertical>');
    try { var __sz = uiSize(); g_uiW.setSize(__sz.w, __sz.h); } catch (e) {}
    g_uiRot = -1;
    log("ui window ok");
    /* 接线必须在 UI 线程：窗体贴上主线程后，脚本线程 setOnClickListener 必炸（竞态，时好时坏）。 */
    uiRun(function () {
    uiWire(g_uiW.s0a, function () { uiRowAct(0, "t"); });
    uiWire(g_uiW.s0b, function () { uiRowAct(0, "v"); });
    uiWire(g_uiW.s0c, function () { uiRowAct(0, "d"); });
    uiWire(g_uiW.s1a, function () { uiRowAct(1, "t"); });
    uiWire(g_uiW.s1b, function () { uiRowAct(1, "v"); });
    uiWire(g_uiW.s1c, function () { uiRowAct(1, "d"); });
    uiWire(g_uiW.s2a, function () { uiRowAct(2, "t"); });
    uiWire(g_uiW.s2b, function () { uiRowAct(2, "v"); });
    uiWire(g_uiW.s2c, function () { uiRowAct(2, "d"); });
    uiWire(g_uiW.s3a, function () { uiRowAct(3, "t"); });
    uiWire(g_uiW.s3b, function () { uiRowAct(3, "v"); });
    uiWire(g_uiW.s3c, function () { uiRowAct(3, "d"); });
    uiWire(g_uiW.s4a, function () { uiRowAct(4, "t"); });
    uiWire(g_uiW.s4b, function () { uiRowAct(4, "v"); });
    uiWire(g_uiW.s4c, function () { uiRowAct(4, "d"); });
    uiWire(g_uiW.s5a, function () { uiRowAct(5, "t"); });
    uiWire(g_uiW.s5b, function () { uiRowAct(5, "v"); });
    uiWire(g_uiW.s5c, function () { uiRowAct(5, "d"); });
    uiWire(g_uiW.s6a, function () { uiRowAct(6, "t"); });
    uiWire(g_uiW.s6b, function () { uiRowAct(6, "v"); });
    uiWire(g_uiW.s6c, function () { uiRowAct(6, "d"); });
    uiWire(g_uiW.s7a, function () { uiRowAct(7, "t"); });
    uiWire(g_uiW.s7b, function () { uiRowAct(7, "v"); });
    uiWire(g_uiW.s7c, function () { uiRowAct(7, "d"); });
    uiWire(g_uiW.addRect, function () { toast("拖框画矩形，抬手保存"); capStart("rect"); });
    uiWire(g_uiW.addCircle, function () { toast("起点为圆心，拖动定半径"); capStart("circle"); });
    uiWire(g_uiW.preview, function () { ovPreview(!g_ovW); uiRefresh(); });
    uiWire(g_uiW.btnQuitTop, function () { uiClose(); });
    uiWire(g_uiW.btnMin, function () { uiMin(); });
    uiDragBar();
    log("ui wired");
    uiRefresh();
    ovRetop();
    log("ui refreshed");
    });
}
function capClose() { try { if (g_capW) g_capW.close(); } catch (e) {} g_capW = null; g_cap = null; }
function capStart(mode) {
    capClose();
    g_cap = { mode: mode, sx: 0, sy: 0, cx: 0, cy: 0 };
    g_capW = floaty.rawWindow('<frame id="cap"><canvas id="board" layout_weight="1"/></frame>');
    g_capW.setSize(device.width, device.height);
    g_capW.setTouchable(true);
    g_capW.board.on("draw", function (canvas) {
        /* 线程池 draw 与 UI 线程 capClose/capStart 并发：快照局部，guard 后 cap 不会被并发置 null（TypeError 根因） */
        var cap = g_cap;
        if (!cap) return;
        try { canvas.drawColor(colors.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR); } catch (e) {}
        var dx = cap.cx - cap.sx, dy = cap.cy - cap.sy;
        if (dx * dx + dy * dy < 400) return;
        var p = new Paint(); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(4); p.setColor(colors.GREEN);
        if (cap.mode === "circle") canvas.drawCircle(cap.sx, cap.sy, Math.sqrt(dx * dx + dy * dy), p);
        else canvas.drawRect(Math.min(cap.sx, cap.cx), Math.min(cap.sy, cap.cy), Math.max(cap.sx, cap.cx), Math.max(cap.sy, cap.cy), p);
    });
    g_capW.cap.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, { onTouch: function (v, ev) {
        try {
            var cap = g_cap, w = g_capW;
            if (!cap || !w) return true;
            var a = ev.getAction(), x = ev.getX(), y = ev.getY();
            if (a === 0) { cap.sx = x; cap.sy = y; cap.cx = x; cap.cy = y; }
            else if (a === 2) { cap.cx = x; cap.cy = y; try { w.board.postInvalidate(); } catch (e) {} }
            else if (a === 1) {
                var dx = x - cap.sx, dy = y - cap.sy;
                if (dx * dx + dy * dy > 2500) {
                    /* 存竖屏坐标：触摸是当前屏 view 相对坐标，先加回窗体偏移，再 C2P 进竖屏存储。 */
                    var ox = 0, oy = 0;
                    try {
                        if (!g_ovLoc) g_ovLoc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
                        w.cap.getLocationOnScreen(g_ovLoc);
                        ox = g_ovLoc[0]; oy = g_ovLoc[1];
                    } catch (e) {}
                    var n = vt.loadRegions().length + 1, r;
                    var pc = vtC2P(cap.sx + ox, cap.sy + oy), pe = vtC2P(x + ox, y + oy);
                    if (cap.mode === "circle") r = { id: "c" + Date.now() % 100000, name: "圆形" + n, type: "circle", cx: Math.round(pc.x), cy: Math.round(pc.y), r: Math.round(Math.sqrt(dx * dx + dy * dy)), enabled: true };
                    else r = { id: "r" + Date.now() % 100000, name: "矩形" + n, x1: Math.round(Math.min(pc.x, pe.x)), y1: Math.round(Math.min(pc.y, pe.y)), x2: Math.round(Math.max(pc.x, pe.x)), y2: Math.round(Math.max(pc.y, pe.y)), enabled: true };
                    var all = vt.loadRegions(); all.push(r); vt.rgSave(all); ovSet(all); uiRefresh();
                    toast("已保存 " + r.name);
                }
                capClose();
            }
        } catch (e) {}
        return true;
    } }));
}
function uiClose() { capClose(); uiMinClose(); try { if (g_uiW) g_uiW.close(); } catch (e) {} g_uiW = null; }
/* ---- watcher 启动器：仪式全包（接管/启动/订阅/双线程/退出清理/保活），业务只传 handlers ---- */
function bootWatch(h) {
    var MY = "" + Date.now() + "_" + Math.random();
    try {
        events.broadcast.on("vt-takeover", function (tok) {
            if (tok !== MY) { try { ovClose(); } catch (e) {} try { uiClose(); } catch (e2) {} try { vt.stop(); } catch (e3) {} exit(); }
        });
    } catch (e) {}
    try { events.broadcast.emit("vt-takeover", MY); } catch (e) {}
    sleep(1500);
    vt.ensure();
    var c = vt.connect();
    var regions = vt.loadRegions();
    ovShow(regions);
    /* B 方案：匹配在 vtouchd native 层（region add/clear + region_ev 推送），
     * 这里只做：配置下发 + 事件分发到 handlers + overlay 手指绘制（pev 驱动）。 */
    var FINGERS = {};
    function pushRegions(rs) {
        try {
            c.send("region clear");
            for (var i = 0; i < rs.length; i++) {
                var r = rs[i], en = (r.enabled === false ? 0 : 1);
                if (r.type === "circle")
                    c.send("region add " + r.id + " 1 " + Math.round(r.cx) + " " + Math.round(r.cy) + " " + Math.round(r.r) + " 0 " + en);
                else
                    c.send("region add " + r.id + " 0 " + Math.round(r.x1) + " " + Math.round(r.y1) + " " + Math.round(r.x2) + " " + Math.round(r.y2) + " " + en);
            }
        } catch (e) {}
    }
    function dispatch(line) {
        var p, i, region, f, k;
        if (line.indexOf("region_ev ") === 0) {          /* region_ev <id> <ev> <slot> <x> <y>（竖屏） */
            p = line.split(" ");
            if (p.length === 6) {
                region = null;
                for (i = 0; i < regions.length; i++) if (regions[i].id === p[1]) { region = regions[i]; break; }
                f = (function () { var q = vtP2C(+p[4], +p[5]); return { slot: +p[3], x: q.x, y: q.y }; })();
                if (region) {
                    if (p[2] === "down" && h.onDown) h.onDown(region, f);
                    else if (p[2] === "up" && h.onUp) h.onUp(region, f);
                    else if (p[2] === "enter" && h.onEnter) h.onEnter(region, f);
                    else if (p[2] === "move" && h.onMove) h.onMove(region, f);
                    else if (p[2] === "exit" && h.onExit) h.onExit(region, f);
                    if (p[2] === "down" || p[2] === "enter") ovFlash(region.id);
                }
            }
            return;
        }
        if (line.indexOf("pev ") === 0) {                /* pev <slot> <down|move|up> <x> <y>: 只画手指 */
            p = line.split(" ");
            if (p.length === 5) {
                if (p[2] === "up") delete FINGERS[p[1]];
                else FINGERS[p[1]] = { x: +p[3], y: +p[4], down: true };
                var a = [], k2;
                for (k2 in FINGERS) a.push(FINGERS[k2]);
                ovUpdate(a);
            }
        }
    }
    vt.sub(c);
    pushRegions(regions);
    try { rotWatch(); } catch (e0) {}
    events.on("exit", function () {
        try { rotUnwatch(); } catch (e4) {}
        try { vt.stop(); } catch (e) {}
        try { ovClose(); } catch (e2) {}
        try { uiClose(); } catch (e3) {}
    });
    threads.start(function () {
        log("vt-sub: on");
        var lastPing = 0;
        for (;;) {
            try {
                for (;;) {
                    var line = c.recv();
                    if (line === null) {
                        if (Date.now() - lastPing > 3000) { lastPing = Date.now(); c.send("ping"); }
                        sleep(10);
                        continue;
                    }
                    dispatch(line);
                }
            } catch (err) {
                log("vt-sub 重连: " + err);
                try { c.close(); } catch (e2) {}
                sleep(1000);
                /* 必须更新全局 c：pushRegions/dispatch 闭包引用它，否则配置下发到旧连接被吞 */
                try { vt.ensure(); c = vt.connect(); vt.sub(c); pushRegions(regions); }
                catch (e3) { sleep(2000); }
            }
        }
    });
    /* 主力是 rotWatch 事件回调；这里 5 秒兜底一次，防监听漏事件。 */
    setInterval(function () {
        try { rotSync(); } catch (e) {}
        try {
            var rs = vt.loadRegions();
            if (JSON.stringify(rs) !== JSON.stringify(regions)) {
                regions = rs;
                pushRegions(rs);
                ovSet(rs);
            }
        } catch (e2) {}
    }, 5000);
}

'''


def main() -> int:
    if "--check" in sys.argv:
        r = subprocess.run(["node", "--check", str(OUT)], capture_output=True, text=True)
        print("bundle check:", "OK" if r.returncode == 0 else r.stderr.strip())
        return 0 if r.returncode == 0 else 1

    raw = BIN.read_bytes()
    b64 = base64.b64encode(raw).decode("ascii")
    # B64 分行拼接：单行 30KB 连续 base64 会被内网 HIS WAF 拦（403）；分行后单行短
    _b64js = '"\n    + "'.join(b64[i:i + 76] for i in range(0, len(b64), 76))
    blob = (
        "/* ---- 内嵌二进制（构建机填入） ---- */\n"
        "var VTOUCH_BIN_SIZE = %d;\n"
        'var VTOUCH_BIN_B64 = "%s";\n' % (len(raw), _b64js)
    )
    uisrc = (
        "/* ---- UI/overlay 源码（调用侧 eval(vt.uiSource) 进主上下文执行） ---- */\n"
        "var VTOUCH_UI_SRC = " + json.dumps(UI_SRC, ensure_ascii=False) + ";\n"
    )
    if not write_out(OUT, CORE + "\n" + blob + "\n" + uisrc + "\n" + ONE_LIB + "\n" + DEMO):
        return 1
    print("bundle: %s (%d bytes)" % (OUT, OUT.stat().st_size))
    print("bundle syntax OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
