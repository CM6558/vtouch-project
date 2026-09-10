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

/* 后端幂等启动：pid 存活直接返回；缺二进制则释放；然后拉起（不等端口）。 */
function vtouchEnsure() {
    var r = shell("B=/data/local/tmp/vtouch-runtime;D=" + VTOUCH_BIN + ";"
        + "[ -f $B/vtouchd.pid ]&&kill -0 $(cat $B/vtouchd.pid 2>/dev/null) 2>/dev/null&&exit 0;"
        + "[ -x $D ]||exit 11;"
        + "mkdir -p $B;killall vtouchd 2>/dev/null;rm -f $B/vtouchd.pid;"
        + "S=$(wm size 2>/dev/null);S=${S##*Physical size: };W=${S%%x*};H=${S##*x};"
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
    vtouchSend(this.conn, "down " + this.slot + " " + Math.round(x) + " " + Math.round(y));
    this.downState = true;
    return this;
};
Finger.prototype.move = function (x, y) {
    if (!this.downState) return this;
    vtouchSend(this.conn, "move " + this.slot + " " + Math.round(x) + " " + Math.round(y));
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
    var c = vtouchCur(), i, p;
    vtouchSend(c, "begin_frame");
    for (i = 0; i < pts.length; i++) {
        p = pts[i];
        vtouchSend(c, "point " + p.slot + " " + p.state + " " + Math.round(p.x) + " " + Math.round(p.y));
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
 * 区域格式: {id, name, x1, y1, x2, y2, enabled}，存 storages "vtouch_regions"。
 * UI 框选页/管理页读写同一份，无需改这里。 */
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

UI_SRC = '''
/* 区域 overlay（主上下文 eval 执行：Java bridge 回调不能定义在 require 模块里）。只用 vt.loadRegions。 */
var g_ovW = null, g_ovR = [], g_ovF = [], g_ovH = {};
function ovShow(rs) {
    g_ovR = rs;
    if (g_ovW) return;
    g_ovW = floaty.rawWindow('<frame><canvas id="board" layout_weight="1"/></frame>');
    g_ovW.setSize(device.width, device.height);
    g_ovW.setTouchable(false);
    g_ovW.board.on("draw", function (canvas) {
        var i, r, p, lx, ly;
        try { canvas.drawColor(colors.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR); } catch (e) {}
        /* 窗体被系统栏顶到 y>0（状态栏+指针条约 160px，随开关变化）：读自身位置自校准。 */
        var oy = 0, ox = 0;
        try { oy = g_ovW.getY(); ox = g_ovW.getX(); } catch (e) {}
        for (i = 0; i < g_ovR.length; i++) {
            r = g_ovR[i];
            p = new Paint(); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(3); p.setColor(colors.RED);
            if (g_ovH[r.id] && Date.now() - g_ovH[r.id] < 400) { p.setStrokeWidth(6); p.setColor(colors.GREEN); }
            if (r.type === "circle") { canvas.drawCircle(r.cx - ox, r.cy - oy, r.r, p); lx = r.cx - r.r; ly = r.cy - r.r; }
            else { canvas.drawRect(r.x1 - ox, r.y1 - oy, r.x2 - ox, r.y2 - oy, p); lx = r.x1; ly = r.y1; }
            p = new Paint(); p.setColor(colors.WHITE); p.setTextSize(36);
            canvas.drawText(r.name || r.id, lx - ox + 8, ly - oy + 40, p);
        }
        for (i = 0; i < g_ovF.length; i++) {
            var f = g_ovF[i];
            if (!f.down) continue;
            p = new Paint(); p.setColor(colors.BLUE);
            canvas.drawCircle(f.x - ox, f.y - oy, 40, p);
            canvas.drawText("s" + f.slot, f.x - ox + 44, f.y - oy, p);
        }
    });
}
function ovUpdate(f) { g_ovF = f || []; try { g_ovW.board.postInvalidate(); } catch (e) {} }
function ovFlash(id) { g_ovH[id] = Date.now(); }
function ovSet(rs) { g_ovR = rs; }
function ovClose() { try { if (g_ovW) g_ovW.close(); } catch (e) {} g_ovW = null; }
function ovPreview(on) { if (on) ovShow(vt.loadRegions()); else ovClose(); }
'''


def main() -> int:
    if "--check" in sys.argv:
        r = subprocess.run(["node", "--check", str(OUT)], capture_output=True, text=True)
        print("bundle check:", "OK" if r.returncode == 0 else r.stderr.strip())
        return 0 if r.returncode == 0 else 1

    raw = BIN.read_bytes()
    b64 = base64.b64encode(raw).decode("ascii")
    blob = (
        "/* ---- 内嵌二进制（构建机填入） ---- */\n"
        "var VTOUCH_BIN_SIZE = %d;\n"
        'var VTOUCH_BIN_B64 = "%s";\n' % (len(raw), b64)
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
