/**
 * vtouch.js —— 最小版 AutoJs6 客户端（只做「虚拟触摸注入」；物理触摸由 daemon 合并转发）。
 *
 * 前置：/data/local/tmp/vtouchd 已在跑（vt.start() 会起）。
 * 用法：
 *   var vt = require("/sdcard/vtouch.js");
 *   vt.start();                                     // 起 daemon（root；已在跑则复用）
 *   var c = vt.connect();                           // 连接 + WS 握手
 *   vt.finger().tap(540, 1200);                     // 自动分配空闲槽
 *   vt.finger(3).down(100, 200).move(120, 240).up();// 显式槽
 *   vt.frame([{slot:0,state:"down",x:100,y:200},    // 多指合并进同一帧
 *             {slot:1,state:"down",x:300,y:200}]);
 *   c.close(); vt.stop();                           // 收尾：停 daemon（释放 EVIOCGRAB）
 *
 * 坐标：daemon 的坐标系是**竖屏逻辑坐标**（启动时用 wm size 归一化）。
 * 本最小版不做旋转换算：横屏时请自行把 device.width/height 与竖屏坐标换算好再传。
 */
"use strict";
var HOST = "127.0.0.1", PORT = 27183, BIN = "/data/local/tmp/vtouchd";
var SEND_LOCK = threads.lock();

function sh(cmd) { return shell(cmd, true); }
function trim(s) { return String(s == null ? "" : s).replace(/^\s+|\s+$/g, ""); }

/* 起 daemon：与设备侧一次性 root shell 里干完（pidof 判活 → wm size 归一化竖屏 → nohup 起） */
function start() {
    if (alive()) return true;
    var cmd = "D=" + BIN + ";[ -f $D ]||exit 11;"
        + "S=$(wm size 2>/dev/null);S=${S##*Physical size: };W=${S%%x*};H=${S##*x};"
        + "if [ $W -gt $H ];then T=$W;W=$H;H=$T;fi;"
        + "kill -9 $(pidof vtouchd) 2>/dev/null;"
        + "nohup $D -w $W -h $H -p " + PORT + " >/data/local/tmp/vtouchd.log 2>&1 </dev/null & echo $!";
    var r = sh(cmd);
    if (r && r.code === 11) throw new Error("设备上缺 " + BIN + " —— 先 adb push 并 chmod 755（或跑 scripts/deploy.sh）");
    for (var i = 0; i < 20 && !alive(); i++) sleep(150);
    if (!alive()) throw new Error("vtouchd 没起来，看 /data/local/tmp/vtouchd.log");
    return true;
}
function alive() { var r = sh("pidof vtouchd"); return !!(r && trim(r.result)); }
/* 停 daemon：进程一退，内核自动解 EVIOCGRAB，物理触摸回到系统直读 */
function stop() { sh("kill -9 $(pidof vtouchd)"); return !alive(); }

function wsKey() {
    var b = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, 16);
    new java.util.Random().nextBytes(b);
    return android.util.Base64.encodeToString(b, android.util.Base64.NO_WRAP);
}
function wsAccept(key) {
    var md = java.security.MessageDigest.getInstance("SHA-1");
    var d = md.digest(new java.lang.String(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").getBytes("UTF-8"));
    return android.util.Base64.encodeToString(d, android.util.Base64.NO_WRAP);
}
function readLine(ins) {
    var sb = new java.lang.StringBuilder();
    for (;;) {
        var b = ins.read();
        if (b < 0) throw new Error("连接断开");
        if (b === 10) break;
        if (b !== 13) sb.append(String.fromCharCode(b));
    }
    return sb.toString();
}
function readFull(ins, n) {
    var buf = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n), off = 0;
    while (off < n) {
        var k = ins.read(buf, off, n - off);
        if (k < 0) throw new Error("连接断开");
        off += k;
    }
    return buf;
}
/* Java byte 有符号：128~255 要折成负数才能写进 byte[] */
function jb(v) { v = v & 255; return v > 127 ? v - 256 : v; }

function connectOnce(timeout) {
    var sock = new java.net.Socket();
    sock.connect(new java.net.InetSocketAddress(HOST, PORT), 3000);
    sock.setSoTimeout(timeout || 5000);
    sock.setTcpNoDelay(true);              /* 突发小包别被 Nagle 推迟 */
    var out = sock.getOutputStream(), ins = sock.getInputStream();
    var key = wsKey();
    var req = "GET / HTTP/1.1\r\nHost: " + HOST + ":" + PORT + "\r\n"
        + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        + "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    out.write(new java.lang.String(req).getBytes("UTF-8"));
    out.flush();
    if (readLine(ins).indexOf("101") < 0) { try { sock.close(); } catch (e) {} throw new Error("握手失败"); }
    var want = wsAccept(key), ok = false, line;
    for (;;) {
        line = readLine(ins);
        if (line.length === 0) break;
        if (line.toLowerCase().indexOf("sec-websocket-accept") === 0 && line.indexOf(want) >= 0) ok = true;
    }
    if (!ok) { try { sock.close(); } catch (e) {} throw new Error("握手 accept 不匹配"); }
    return { sock: sock, out: out, ins: ins, mask: new java.util.Random() };
}
function connect(timeout) {
    timeout = timeout || 10000;
    var t0 = Date.now(), last = null;
    for (;;) {
        try { return attach(connectOnce(timeout)); }
        catch (e) {
            last = e;
            if (Date.now() - t0 > timeout) throw new Error("连不上 vtouchd: " + last);
            sleep(300);
        }
    }
}
/* 给裸 socket 挂上 send/recv/drain/close（客户端帧必须掩码） */
function attach(conn) {
    conn.send = function (text) {
        SEND_LOCK.lock();
        try {
            var data = new java.lang.String(text).getBytes("UTF-8");
            var n = data.length, i;
            var m = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, 4);
            conn.mask.nextBytes(m);
            var h = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n < 126 ? 2 : 4);
            h[0] = jb(0x81);
            if (n < 126) h[1] = jb(0x80 | n);
            else { h[1] = jb(0x80 | 126); h[2] = jb(n >> 8); h[3] = jb(n); }
            conn.out.write(h); conn.out.write(m);
            var masked = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n);
            for (i = 0; i < n; i++) masked[i] = jb(data[i] ^ m[i & 3]);
            conn.out.write(masked);
            conn.out.flush();
        } finally { SEND_LOCK.unlock(); }
    };
    /* 非阻塞取一条文本消息；没数据返回 null（不要用长 sleep 轮询，会积压） */
    conn.recv = function () {
        if (conn.ins.available() < 2) return null;
        var h = readFull(conn.ins, 2);
        var len = h[1] & 127, i;
        if (len === 126) { var e = readFull(conn.ins, 2); len = ((e[0] & 255) << 8) | (e[1] & 255); }
        else if (len === 127) throw new Error("帧过大");
        if ((h[0] & 15) === 8) { try { conn.sock.close(); } catch (e2) {} throw new Error("服务端关闭"); }
        var p = readFull(conn.ins, len), cs = [];
        for (i = 0; i < len; i++) cs.push(String.fromCharCode(p[i] & 255));
        return decodeURIComponent(escape(cs.join("")));
    };
    conn.drain = function () { try { while (conn.recv() !== null) {} } catch (e) {} };
    conn.close = function () { try { conn.sock.close(); } catch (e) {} };
    conn.cmd = function (line) {          /* 发一条命令并等一行回包（调试用） */
        conn.drain();
        conn.send(line);
        var dl = Date.now() + 500;
        for (;;) {
            var s = conn.recv();
            if (s) return s;
            if (Date.now() > dl) return null;
            sleep(5);
        }
    };
    conn.fingers = {};
    return conn;
}

/* Finger：down/move/up/tap/swipe/frame/state；slot 省略 = 自动挑 0..9 里第一个空闲的 */
function Finger(conn, slot) { this.conn = conn; this.slot = slot; this.downState = false; }
Finger.prototype.down = function (x, y) {
    this.conn.send("down " + this.slot + " " + Math.round(x) + " " + Math.round(y));
    this.downState = true; return this;
};
Finger.prototype.move = function (x, y) {
    if (!this.downState) return this;
    this.conn.send("move " + this.slot + " " + Math.round(x) + " " + Math.round(y));
    return this;
};
Finger.prototype.up = function () {
    if (!this.downState) return this;
    this.conn.send("up " + this.slot);
    this.downState = false; return this;
};
Finger.prototype.tap = function (x, y, ms) {
    this.down(x, y); sleep(ms == null ? 60 : ms); return this.up();   /* 按住时长由脚本控制 */
};
Finger.prototype.swipe = function (x1, y1, x2, y2, ms) {
    var total = ms == null ? 300 : ms, n = Math.max(2, Math.round(total / 16.7)), i;
    this.down(x1, y1);
    for (i = 1; i <= n; i++) { sleep(total / n); this.move(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n); }
    return this.up();
};
Finger.prototype.state = function () { return this.downState ? "down" : "up"; };
function finger(conn, slot) {
    if (!conn.fingers) conn.fingers = {};
    if (slot === undefined || slot === null) {
        for (var i = 0; i <= 9; i++) if (!conn.fingers[i] || !conn.fingers[i].downState) { slot = i; break; }
        if (slot === undefined || slot === null || slot > 9) throw new Error("无空闲 slot");
    } else {
        slot = Math.round(slot);
        if (slot < 0 || slot > 9) throw new Error("slot 0~9");
    }
    if (!conn.fingers[slot]) conn.fingers[slot] = new Finger(conn, slot);
    return conn.fingers[slot];
}
/* 多指合并进同一帧（begin_frame → point×N → end_frame） */
function frame(conn, pts) {
    var i, p;
    conn.send("begin_frame");
    try {
        for (i = 0; i < pts.length; i++) {
            p = pts[i];
            conn.send("point " + p.slot + " " + p.state + " " + Math.round(p.x) + " " + Math.round(p.y));
            if (conn.fingers && conn.fingers[p.slot]) conn.fingers[p.slot].downState = p.state !== "up";
        }
    } finally {
        conn.send("end_frame");
    }
}

module.exports = {
    start: start, stop: stop, alive: alive,
    connect: connect, finger: finger, frame: frame,
    BIN: BIN, HOST: HOST, PORT: PORT
};
