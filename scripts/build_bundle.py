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
    conn.close = function () { try { conn.sock.close(); } catch (e) {} };
    return conn;
}

function vtouchSend(c, line) { c.drain(); c.send(line); }
function vtouchReset() { vtouchSend(vtouchCur(), "reset"); }
function vtouchStop() {
    shell("killall vtouchd 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/vtouchd.pid", true);
}

/* ---- 入口：仪式全包，业务只写触摸逻辑 ----
 * vt.run(function () { vt.finger().tap(540, 1200); });
 * 主线程必须直接返回（Looper 泵事件），阻塞只在业务线程。
 * run 内为当前连接，finger/frame/reset 直接用，不用传 c。 */
var CURR = null;
function vtouchCur() {
    if (!CURR) throw new Error("请在 vt.run(function(){...}) 内调用");
    return CURR;
}
function vtouchRun(fn) {
    device.wakeUpIfNeeded();
    threads.start(function () {
        var c = null;
        try {
            device.keepScreenOn(60 * 1000);
            vtouchEnsure();
            c = vtouchConnect();
            CURR = c;
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
    BIN: VTOUCH_BIN,
    HOST: VTOUCH_HOST,
    PORT: VTOUCH_PORT
};
"""


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
    OUT.write_text(CORE + "\n" + blob + "\n" + DEMO, encoding="utf-8", newline="\n")
    print("bundle: %s (%d bytes, bin %d bytes)" % (OUT, OUT.stat().st_size, len(raw)))
    r = subprocess.run(["node", "--check", str(OUT)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr.strip())
        return 1
    print("bundle syntax OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
