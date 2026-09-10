/* ============================================================================
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
    var req = "GET / HTTP/1.1\r\nHost: " + VTOUCH_HOST + ":" + VTOUCH_PORT + "\r\n"
        + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        + "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
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

/* ---- 内嵌二进制（构建机填入） ---- */
var VTOUCH_BIN_SIZE = 26272;
var VTOUCH_BIN_B64 = "f0VMRgIBAQAAAAAAAAAAAAMAtwABAAAAyCYAAAAAAABAAAAAAAAAAGBgAAAAAAAAAAAAAEAAOAAL"
    + "AEAAGQAXAAYAAAAEAAAAQAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAaAIAAAAAAABoAgAAAAAAAAgA"
    + "AAAAAAAAAwAAAAQAAACoAgAAAAAAAKgCAAAAAAAAqAIAAAAAAAAVAAAAAAAAABUAAAAAAAAAAQAA"
    + "AAAAAAABAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMgWAAAAAAAAyBYAAAAAAAAAEAAA"
    + "AAAAAAEAAAAFAAAAyBYAAAAAAADIJgAAAAAAAMgmAAAAAAAAeDQAAAAAAAB4NAAAAAAAAAAQAAAA"
    + "AAAAAQAAAAYAAABASwAAAAAAAEBrAAAAAAAAQGsAAAAAAADAAgAAAAAAAMAEAAAAAAAAABAAAAAA"
    + "AAABAAAABgAAAABOAAAAAAAAAH4AAAAAAAAAfgAAAAAAADQAAAAAAAAA9A0AAAAAAAAAEAAAAAAA"
    + "AAIAAAAGAAAAQEsAAAAAAABAawAAAAAAAEBrAAAAAAAAYAEAAAAAAABgAQAAAAAAAAgAAAAAAAAA"
    + "UuV0ZAQAAABASwAAAAAAAEBrAAAAAAAAQGsAAAAAAADAAgAAAAAAAMAEAAAAAAAAAQAAAAAAAABQ"
    + "5XRkBAAAAOwQAAAAAAAA7BAAAAAAAADsEAAAAAAAANwAAAAAAAAA3AAAAAAAAAAEAAAAAAAAAFHl"
    + "dGQGAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAA"
    + "AAQAAADAAgAAAAAAAMACAAAAAAAAwAIAAAAAAACYAAAAAAAAAJgAAAAAAAAABAAAAAAAAAAvc3lz"
    + "dGVtL2Jpbi9saW5rZXI2NAAAAAAIAAAAhAAAAAEAAABBbmRyb2lkABgAAAByMjdkAAAAAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMTM3NTA3"
    + "MjQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAASAAAAAAAAAAAAAAAAAAAAAAAAAA0AAAASAAAA"
    + "AAAAAAAAAAAAAAAAAAAAABoAAAASAAAAAAAAAAAAAAAAAAAAAAAAACwAAAASAAAAAAAAAAAAAAAA"
    + "AAAAAAAAADgAAAASAAAAAAAAAAAAAAAAAAAAAAAAAEIAAAASAAAAAAAAAAAAAAAAAAAAAAAAAEkA"
    + "AAASAAAAAAAAAAAAAAAAAAAAAAAAAFAAAAASAAAAAAAAAAAAAAAAAAAAAAAAAFgAAAASAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAF8AAAARAAAAAAAAAAAAAAAAAAAAAAAAAGYAAAASAAAAAAAAAAAAAAAAAAAA"
    + "AAAAAG4AAAASAAAAAAAAAAAAAAAAAAAAAAAAAHUAAAASAAAAAAAAAAAAAAAAAAAAAAAAAHsAAAAS"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAIQAAAASAAAAAAAAAAAAAAAAAAAAAAAAAIkAAAASAAAAAAAAAAAA"
    + "AAAAAAAAAAAAAI8AAAASAAAAAAAAAAAAAAAAAAAAAAAAAJYAAAASAAAAAAAAAAAAAAAAAAAAAAAA"
    + "AKEAAAASAAAAAAAAAAAAAAAAAAAAAAAAAKcAAAASAAAAAAAAAAAAAAAAAAAAAAAAALEAAAASAAAA"
    + "AAAAAAAAAAAAAAAAAAAAALYAAAASAAAAAAAAAAAAAAAAAAAAAAAAAL0AAAASAAAAAAAAAAAAAAAA"
    + "AAAAAAAAAMQAAAASAAAAAAAAAAAAAAAAAAAAAAAAAM0AAAASAAAAAAAAAAAAAAAAAAAAAAAAANIA"
    + "AAASAAAAAAAAAAAAAAAAAAAAAAAAANcAAAASAAAAAAAAAAAAAAAAAAAAAAAAAN8AAAASAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAOQAAAASAAAAAAAAAAAAAAAAAAAAAAAAAOsAAAASAAAAAAAAAAAAAAAAAAAA"
    + "AAAAAPIAAAASAAAAAAAAAAAAAAAAAAAAAAAAAP0AAAASAAAAAAAAAAAAAAAAAAAAAAAAAAMBAAAS"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAAgBAAASAAAAAAAAAAAAAAAAAAAAAAAAAA8BAAASAAAAAAAAAAAA"
    + "AAAAAAAAAAAAABYBAAASAAAAAAAAAAAAAAAAAAAAAAAAACIBAAASAAAAAAAAAAAAAAAAAAAAAAAA"
    + "ACoBAAASAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgACAAIAAgACAAIAAgACAAIAAgACAAIAAgACAAIA"
    + "AgACAAIAAgACAAIAAgACAAIAAgACAAIAAgACAAIAAgACAAIAAgACAAIAAgACAAAAAQABADMBAAAQ"
    + "AAAAAAAAAGMNBQAAAAIAOwEAAAAAAAABAAAAJwAAAAEAAAAaAAAAAAAAAAAAAAAAAAAAAF9fbGli"
    + "Y19pbml0AF9fY3hhX2F0ZXhpdABfX3JlZ2lzdGVyX2F0Zm9yawBzaWdlbXB0eXNldABzaWdhY3Rp"
    + "b24Ac2lnbmFsAHN0cmNtcABfX2Vycm5vAHN0cnRvbABzdGRlcnIAZnByaW50ZgBtZW1zZXQAY2xv"
    + "c2UAc25wcmludGYAb3BlbgBpb2N0bABzb2NrZXQAc2V0c29ja29wdABmY250bABpbmV0X3B0b24A"
    + "YmluZABsaXN0ZW4AZndyaXRlAHN0cmVycm9yAHBvbGwAcmVhZABhY2NlcHQ0AHJlY3YAbWVtY3B5"
    + "AHN0cmxlbgBzdHJjYXNlY21wAHdyaXRlAHNlbmQAc3Ryc3RyAG1lbWNocgBzdHJuY2FzZWNtcABz"
    + "dHJjc3BuAHN0cnRva19yAGxpYmMuc28ATElCQwBsaWJkbC5zbwAAAACgbAAAAAAAAAMEAAAAAAAA"
    + "yCYAAAAAAACobAAAAAAAAAMEAAAAAAAAyCYAAAAAAACwbAAAAAAAAAMEAAAAAAAAyCcAAAAAAAAI"
    + "fgAAAAAAAAMEAAAAAAAAdCcAAAAAAAC4bAAAAAAAAAEEAAAKAAAAAAAAAAAAAADYbAAAAAAAAAIE"
    + "AAABAAAAAAAAAAAAAADgbAAAAAAAAAIEAAACAAAAAAAAAAAAAADobAAAAAAAAAIEAAADAAAAAAAA"
    + "AAAAAADwbAAAAAAAAAIEAAAEAAAAAAAAAAAAAAD4bAAAAAAAAAIEAAAFAAAAAAAAAAAAAAAAbQAA"
    + "AAAAAAIEAAAGAAAAAAAAAAAAAAAIbQAAAAAAAAIEAAAHAAAAAAAAAAAAAAAQbQAAAAAAAAIEAAAI"
    + "AAAAAAAAAAAAAAAYbQAAAAAAAAIEAAAJAAAAAAAAAAAAAAAgbQAAAAAAAAIEAAALAAAAAAAAAAAA"
    + "AAAobQAAAAAAAAIEAAAMAAAAAAAAAAAAAAAwbQAAAAAAAAIEAAANAAAAAAAAAAAAAAA4bQAAAAAA"
    + "AAIEAAAOAAAAAAAAAAAAAABAbQAAAAAAAAIEAAAPAAAAAAAAAAAAAABIbQAAAAAAAAIEAAAQAAAA"
    + "AAAAAAAAAABQbQAAAAAAAAIEAAARAAAAAAAAAAAAAABYbQAAAAAAAAIEAAASAAAAAAAAAAAAAABg"
    + "bQAAAAAAAAIEAAATAAAAAAAAAAAAAABobQAAAAAAAAIEAAAUAAAAAAAAAAAAAABwbQAAAAAAAAIE"
    + "AAAVAAAAAAAAAAAAAAB4bQAAAAAAAAIEAAAWAAAAAAAAAAAAAACAbQAAAAAAAAIEAAAXAAAAAAAA"
    + "AAAAAACIbQAAAAAAAAIEAAAYAAAAAAAAAAAAAACQbQAAAAAAAAIEAAAZAAAAAAAAAAAAAACYbQAA"
    + "AAAAAAIEAAAaAAAAAAAAAAAAAACgbQAAAAAAAAIEAAAbAAAAAAAAAAAAAACobQAAAAAAAAIEAAAc"
    + "AAAAAAAAAAAAAACwbQAAAAAAAAIEAAAdAAAAAAAAAAAAAAC4bQAAAAAAAAIEAAAeAAAAAAAAAAAA"
    + "AADAbQAAAAAAAAIEAAAfAAAAAAAAAAAAAADIbQAAAAAAAAIEAAAgAAAAAAAAAAAAAADQbQAAAAAA"
    + "AAIEAAAhAAAAAAAAAAAAAADYbQAAAAAAAAIEAAAiAAAAAAAAAAAAAADgbQAAAAAAAAIEAAAjAAAA"
    + "AAAAAAAAAADobQAAAAAAAAIEAAAkAAAAAAAAAAAAAADwbQAAAAAAAAIEAAAlAAAAAAAAAAAAAAD4"
    + "bQAAAAAAAAIEAAAmAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAACCh"
    + "BwAAAAAAASNFZ4mrze/+3LqYdlQyEHdlYnNvY2tldABzdWIAbW92ZQANCg0KAHBvbmcAdnRvdWNo"
    + "ZDogbG9naWNhbCBkaXNwbGF5IHNpemUgcmVxdWlyZWQgKC13IHdpZHRoIC1oIGhlaWdodCkKADEy"
    + "Ny4wLjAuMQBlcnIgZW1wdHkALXcAcGV2ICVkICVzICVkICVkAAPqAHZ0b3VjaGQ6IGtpY2tpbmcg"
    + "b2xkIHdzIGNsaWVudAoALXAAcG9pbnQAdXAAA+8AZXJyIGxpbmUgdG9vIGxvbmcAVXBncmFkZQBl"
    + "cnIgcG9pbnQAcGluZwBTZWMtV2ViU29ja2V0LVZlcnNpb24AIAkAR0VUIABTZWMtV2ViU29ja2V0"
    + "LUtleQANCgBDb25uZWN0aW9uAHJlc2V0AHJlcyAlZCAlZCByYXcgJWQgJWQgJWQgJWQAdnRvdWNo"
    + "ZDogZGV2PSVzIHBoeXM9JWQgdmlydD0lZCB3cz0xMjcuMC4wLjE6JWQgc2l6ZT0lZHglZAoAdnRv"
    + "dWNoZDogd3MgaGFuZHNoYWtlIGZhaWxlZAoALXYAQUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVph"
    + "YmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ejAxMjM0NTY3ODkrLwAtaAAyNThFQUZBNS1FOTE0LTQ3"
    + "REEtOTVDQS1DNUFCMERDODVCMTEAZXJyIGZyYW1lAHZ0b3VjaGQ6IHdzIGNsaWVudCBodW5nIHVw"
    + "CgAtcwAvZGV2L3VpbnB1dABkb3duAGJlZ2luX2ZyYW1lAHZ0b3VjaGQ6IG5vIFR5cGUtQiB0b3Vj"
    + "aHNjcmVlbiBmb3VuZAoAL2Rldi9pbnB1dC9ldmVudCVkADEzAG9rAHZ0b3VjaGQ6IHVpbnB1dCBz"
    + "ZXR1cCBmYWlsZWQ6ICVzCgByZXMAdW5zdWIAZW5kX2ZyYW1lAGVyciB1bmtub3duAHZ0b3VjaGQ6"
    + "IHdzIGNsaWVudCBjb25uZWN0ZWQKAHZ0b3VjaGQ6IHdzIGNsaWVudCBkcm9wcGVkCgBIVFRQLzEu"
    + "MSAxMDEgU3dpdGNoaW5nIFByb3RvY29scw0KVXBncmFkZTogd2Vic29ja2V0DQpDb25uZWN0aW9u"
    + "OiBVcGdyYWRlDQpTZWMtV2ViU29ja2V0LUFjY2VwdDogJXMNCg0KAHVzYWdlOiAlcyAtdyB3aWR0"
    + "aCAtaCBoZWlnaHQgWy12IHNsb3RzXSBbLXAgcG9ydF0KAHZ0b3VjaC1tZXJnZWQAAAAAAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    + "ARsDO9gAAAAaAAAA8BUAAPAAAABIFgAAEAEAAFwWAAAkAQAAeBYAADgBAACIFgAATAEAANwWAAB8"
    + "AQAARCsAAMQBAABUKwAA2AEAAOwrAAAEAgAAHC4AAEACAACwLgAAbAIAAFQvAAC4AgAAoDUAAAQD"
    + "AAAENgAAMAMAAGQ2AABEAwAAwDYAAHADAAD8NwAAqAMAAIQ4AADwAwAA3DkAADQEAACYOgAAcAQA"
    + "ACg9AAC4BAAAdD4AAOwEAABMPwAAAAUAAOQ/AABMBQAALEYAAIgFAABYRwAAwAUAABAAAAAAAAAA"
    + "AXpSAAF8HgEbDB8AHAAAABgAAAD4FAAAWAAAAABELUQOQEgMHRCeAp0EAAAQAAAAOAAAADAVAAAU"
    + "AAAAAAAAABAAAABMAAAAMBUAABwAAAAAAAAAEAAAAGAAAAA4FQAAEAAAAAAAAAAsAAAAdAAAADQV"
    + "AABUAAAAAEQtRA4gSAwdIJMClASeBp0IdAwfIEgOAEQt09Te3QBEAAAApAAAAFgVAABoFAAAAEQO"
    + "YFgMHWCTApQElQaWCJcKmAyZDpoQmxKcFJ4WnRgKAzgJDB9gWA4A09TV1tfY2drb3N7dRAsQAAAA"
    + "7AAAAHgpAAAQAAAAAAAAACgAAAAAAQAAdCkAAJgAAAAARA4gSAwdIJMEngadCAKADB8gSA4A097d"
    + "AAAAOAAAACwBAADgKQAAMAIAAABEDkBQDB1AkwKUBJUGlgiXCpwMng6dEAoCsAwfQFAOANPU1dbX"
    + "3N7dRAsAKAAAAGgBAADUKwAAlAAAAABEDiBIDB0gkwSeBp0IAnwMHyBIDgDT3t0AAABIAAAAlAEA"
    + "ADwsAACkAAAAAFAOQFAMHUCTApQElQaWCJcMng6dEAp8DB8ACBMIFAgVCBYIFwgeCB1IC2wMH0BQ"
    + "DgDT1NXW197dAAAASAAAAOABAACULAAATAYAAABEDoABXAwdYJMClASVBpYIlwqYDJkOmhCbEpwU"
    + "nhadGAoDLAUMH4ABXA4A09TV1tfY2drb3N7dRAsAACgAAAAsAgAAlDIAAGQAAAAARA5ATAwdIJMC"
    + "lASeBp0IAkQMH0BMDgDT1N7dEAAAAFgCAADMMgAAYAAAAAAAAAAoAAAAbAIAABgzAABcAAAAAEQO"
    + "QEwMHSCTApQEngadCHwMH0BMDgDT1N7dADQAAACYAgAASDMAADwBAAAAVA5QVAwdQJMClASVBpYI"
    + "lwqYDJ4OnRAC/AwfUFQOANPU1dbX2N7dRAAAANACAABMNAAAiAAAAABIDjBMDB0wkwKUBJUGlgie"
    + "Cp0MCgJYDB8ACBMIFAgVCBYIHggdSAtEDB8wTA4A09TV1t7dAAAAQAAAABgDAACMNAAAWAEAAABE"
    + "DlBUDB1QkwKUBJUGlgiXCpgMmQ6aEJ4SnRQKAwwBDB9QVA4A09TV1tfY2dre3UQLAAA4AAAAXAMA"
    + "AKA1AAC8AAAAAEQOQFAMHUCTApQElQaWCJcKmAyeDp0QCgKMDB9AUA4A09TV1tfY3t1ECwBEAAAA"
    + "mAMAACA2AACQAgAAAFQOoANcDB1gkwKUBJUGlgiXCpgMmQ6aEJsSnBSeFp0YA0ACDB+gA1wOANPU"
    + "1dbX2Nna29ze3QAwAAAA4AMAAGg4AABMAQAAAEQOsAFQDB0wkwKUBJUIngqdDAMkAQwfsAFQDgDT"
    + "1NXe3QAAEAAAABQEAACAOQAA2AAAAAAAAABIAAAAKAQAAEQ6AACYAAAAAEgOQFAMHUCTApQElQaW"
    + "CJcMng6dEAoCYAwfAAgTCBQIFQgWCBcIHggdSAtEDB9AUA4A09TV1tfe3QAAOAAAAHQEAACQOgAA"
    + "SAYAAABEDmBUDB1AkwKUBJUGlgiXDJ4OnRAKAyACDB9gVA4A09TV1tfe3UQLAAAANAAAALAEAACc"
    + "QAAALAEAAABEDkBQDB1AkwKUBJUGlgiXDJ4OnRADBAEMH0BQDgDT1NXW197dAAAUAAAA6AQAAJBB"
    + "AACIAAAAAAAAAAAAAAAAAAAAnyQD1R0AgNIeAIDS4AMAkQEAABQ/IwPV/wMB0f17A6n9wwCRKAAA"
    + "kCkAAJAA5ABvCFFG+SlVRvkoAQjLHwUA8eCDAK3gA4A9iwAAVB8gA9VItwIQ6AsA+SIAAJDjAwCR"
    + "4QMfqkJYRvlwDACUXyQD1WAAALTwAwCqAAIf1sADX9ZfJAPV4QMAqh8gA9UA//8QHyAD1eK2AhBo"
    + "DAAUXyQD1R8gA9VjtgIQaAwAFD8jA9X9e76p9E8Bqf0DAJEzAACQKAAAkHNSRvkIVUb5CAET6wAB"
    + "AFQI/UOTaQ4IixQFANEpgV/4IAE/1ugDFKp0//+19E9Bqf17wqi/IwPVwANf1v17uqn8bwGp+mcC"
    + "qfhfA6n2VwSp9E8Fqf0DAJH/C0DR/0MS0QDkAG8oAIBS6f//0CmBM5HoLwC56MMAkSEBwD30AwAq"
    + "CQAAsCnBMJEAQQCR8wMBquEHgD3ggwGt6R8A+T4MAJThwwCR4AGAUuIDH6o+DACU4cMAkUAAgFLi"
    + "Ax+qOgwAlKABgFIhAIBSOwwAlJ8KAHFrFwBU9f//0LVSOpH2///Q1ro1kff//9D3YjuROQCAUvj/"
    + "/9AYkzaRBwAAFCgAALAAQQ65+gMZKlkHABE/AxRrahUAVHvaefjhAxWq4AMbqikMAJSgAgA1KH9A"
    + "kxoFAJFfAxRrKgIAVHt6evi7AQC0aANAOWgBADQjDACU4QdAkfwDAKofAAC5IcARkeADG6pCAYBS"
    + "IAwAlIgDQLlIDQA0e3p6+PkDGirgAxuq4QMWqhEMAJSAAgA1OgcAEV8DFGsqAgBUe9p6+PkDGiq7"
    + "AQC0aANAOWgBADQLDACU4QdAkfwDAKofAAC5IcARkeADG6pCAYBSCAwAlIgDQLmICwA0e9p6+OAD"
    + "G6rhAxeq+gsAlIACADU6BwARXwMUayoCAFR72nr4+QMaKrsBALRoA0A5aAEANPQLAJThB0CR/AMA"
    + "qh8AALkhwBGR4AMbqkIBgFLxCwCUiANAuegJADR72nr44AMbquEDGKrjCwCUgAIANToHABFfAxRr"
    + "KgIAVHvaevj5AxoquwEAtGgDQDloAQA03QsAlOEHQJH8AwCqHwAAuSHAEZHgAxuqQgGAUtoLAJSI"
    + "A0C5iAgANHvaevjgAxuq4f//0CGYPJHLCwCUgAAANToHABFfAxRrC/P/VPoDACrgAxuq4QMVqsML"
    + "AJRA8v804AMbquEDFqq/CwCUwPH/NOADG6rhAxequwsAlEDx/zTgAxuq4QMYqrcLAJTa8P80oPD/"
    + "NCgAAJDh///wIaQBkQhdRvliAkD5AAFA+boLAJR9//8X6DtK+QgBQDmo8v81HwQA8Wvy/1QfhADx"
    + "KvL/VCgAALAAGQ65dP//F+g7SvkIAUA5aPT/NR8IAPEr9P9UCNSQUigAoHIfAAjrDe3/VJz//xfo"
    + "O0r5CAFAOQj2/zUfCADxy/X/VAjUkFIoAKByHwAI60z1/1QoAACwAEUOuV3//xfoO0r5CAFAOWj3"
    + "/zUfBADxK/f/VB9AQPHq9v9UKAAAsAAlDrlT//8XKAAAsAhBTrkfCQBxqzEAVCgAALAIRU65HwUA"
    + "cS0xAFQaANDS862IUjmhiFIbAIFSIAAAsAAgOZHhAx8qAqCAUhpM4PITA7ByGQCwchsBoHJ+CwCU"
    + "IAAA0AAgDZHhAx8qAlCAUnkLAJT0Ax8q9f//0LWyPZE3AADQOAAA0DwAANAGAAAU4AMWKnQLAJSU"
    + "BgARnwIBcaAoAFTgwwGRAQCCUuIDFarjAxQqcAsAlAEAgVLgwwGR/zsJ+QEBoHJvCwCUYP7/N+IH"
    + "QJEhXwAR9gMAKkLAAZFtCwCUYP3/N+IHQJFhpIhS4AMWKkLACZEBAbByZgsAlID8/zfiB0CRIaGI"
    + "UuADFipCwAGRAQCwcl8LAJSg+/836DtJ+V8DKOpB+/9U4gdAkeGtiFLgAxYqQsARkQEDsHJVCwCU"
    + "YPr/N+h3VLko+v836XtUuT/9AHHM+f9UKAEIS+IHQJFhGgARCAUAEULAEZHgAxYq6MoFuUcLAJSg"
    + "+P836HtUuel3VLkfAQlrLfj/VOIHQJFhHgAR4AMWKkLAEZEJ3wW5iOcFuTsLAJQg9/836HtUuel3"
    + "VLkfAQlrrfb/VDkAANAzAADQ4AMWKinjBblo6gW5JAsAlCkAALDoykW5OgAA0CkZTrng///QAKQ8"
    + "kWEHABEoAQgLCQyAUh+BAXEIsYkaSM8FuR8LAJQ1AACwoC4OuSAg+Df2rIpS4gMfKpYAqHLUDgBR"
    + "4QMUKhoLAJRgHvg3oC5OueEDFCoiAIBSFQsAlMAd+DegLk654QMUKmIAgFIQCwCUIB34N6AuTrnU"
    + "CgBRQimAUuEDFCoKCwCUYBz4N6AuTrnhAxQqoiiAUgULAJTAG/g3oC5OucEeABEiAIBSAAsAlCAb"
    + "+DegLk654ayKUuIFgFKBAKhy+goAlGAa+DegLk654ayKUiIHgFKBAKhy9AoAlKAZ+DegLk654ayK"
    + "UqIGgFKBAKhy7goAlOAY+DegLk654ayKUsIGgFKBAKhy6AoAlCAY+DegLk654ayKUuIGgFKBAKhy"
    + "4goAlGAX+DcA5ABv6QdAkR8gA9UIEv8QKcERkaAuTrkCAcA94gdAkWGgilL7B0CRQsARkYELqHLg"
    + "H4U9e8MRkeAjhT3gJ4U94CuFPeAvhT0gwYQ8AYFArSKBgDwhgYE8AQ3APSCBgjwA8cM8yACAUiGB"
    + "gzwgcYQ86OMoecMKAJSAE/g3SM9FuaAuTrniB0CRgaCKUukFgFJCwAmRCAUAUYEDqHL/Own5/z8J"
    + "+f+LErn/Qwn56eMkeeh/ErmzCgCUgBH4N6AuTrniB0CRgaCKUigHgFLp/59SQsAJkYEDqHLo4yR5"
    + "6X8SuagKAJQgEPg3Cd9FuYrnRbniB0CRoC5OuYGgilKoBoBSQsAJkYEDqHLo4yR56XsSuep/Ermb"
    + "CgCUgA74NynjRblq6kW54gdAkaAuTrmBoIpSyAaAUkLACZGBA6hy6OMkeel7ErnqfxK5jgoAlOAM"
    + "+DegLk654gdAkYGgilLoBoBSSQCAUkLACZGBA6hy6OMkeel/ErmDCgCUgAv4N6AuTrkhoIpSfwoA"
    + "lAAL+DcBAIFS4MMBkQEBoHJ2CgCUKAAAsAAdDrlgDfg3KACAUkAAgFIhAIBS4gMfKuhzErlVAIBS"
    + "dAoAlCkAAJAADfg34wdAkSEAgFJCAIBSY8AJkYQAgFL0AwAqbwoAlOADFCphAIBS4gMfKm8KAJQC"
    + "AA0y4AMUKoEAgFJrCgCUMwAAkOkHQJHh//+wIWg1kWhKXHkpwRGRIhEAkUAAgFJ/QwD4CAnAWv9/"
    + "FLkIfRBT9eMoeejnKHlgCgCUHwQAcaEIAFThB0CR4AMUKgICgFIhwBGRXQoAlOAH+DfgAxQqAQGA"
    + "Ul0KAJRgB/g3NQAAkDoAAJDBXj9RoB5OuSIAgFJUIw65PQoAlGAH+DbXAgCUoACAUgoAABQIAADw"
    + "4P//sAAYPZEIXUb5oQSAUgMBQPkiAIBSTQoAlEAAgFL/C0CR/0MSkfRPRan2V0Sp+F9DqfpnQqn8"
    + "b0Gp/XvGqMADX9agLk65QaCKUiQKAJSgLk65FgoAlAgAgBKoLg65CAAA8AhdRvkTAUD5AAoAlAAA"
    + "QLk6CgCU4gMAquH//7AhFD6R4AMTqgEKAJRgAIBS5P//FwgAAPAfIAPVYNz+MAhdRvmBB4BS2v//"
    + "F6gCAJSAAIBS2///F+ADFCr8CQCUKQAAkAgAgBIoIQ65oAIAlMAAgFLT//8XQCNOueOzAJHBAIBS"
    + "IgCAUoQAgFI0AIBSAwoAlAgAAPApAACQ4f//sCHoOJEIXUb548pFuSQZTrllJk654sMBkQABQPko"
    + "AACQBkFOuSgAAJAHRU652AkAlCkAALAo0UW5iAAANIQCAJTgAx8qt///F+j//7DzAxqq+gMJqgBp"
    + "Rv22gwDR4AcA/QcAABTCCQCUCABAuR8RAHFh/v9USNNFuSj+/zWoHk65KgOAUmkiTrnoKwspKAAA"
    + "kAgpTrnpUwwp6f+fUugnDSmIAPg3YQCAUurbAHkCAAAUQQCAUuBjAZECfYBS7AkAlCD9/zfov0B5"
    + "KCAANqAeTrnhB0CRAgOAUiHACZHoCQCU+QMAqh9gAPHhHQBU8wMfKhAAABToh1K56cpFuR8BCWsJ"
    + "AIASBLFJeikAALAIwZ8aKO0FuaAeTrnhB0CRAgOAUiHACZHWCQCUH2AA8QEHAFToA2V56QdleR8N"
    + "AHFhAABUP70AcYD9/1QfDQBxoQIAVCgAALDqykW5CO2FuR8BCmuq/f9UP9UAcWADAFQ/2QBxQAIA"
    + "VD/lAHHh/P9U6YdSuYkD+DeLAoBSKgAAkEohOZEIKSubCQEAuRQNALne//8XCAEJKoj7/zUpAwCU"
    + "oBj4NzMAgFLY//8XigKAUikAAJApITmRCCUqm+mHUrkJCQC50f//F4oCgFIpAACQKSE5kQglKpvp"
    + "h1K5CQUAucr//xeKAoBSKQAAkCkhOZEIJSqb4AdA/QDBAPzD//8X+QMAqpMUADQoAACQOgCAUggp"
    + "TrnIE/g3KAAAsAhhVzloEwA06MpFuTMAALAwAACwEMIXkfH//7AxOjSRHwUAcWsSAFT0Ax+qNQAA"
    + "kLVCOZEKAAAU6MpFuTAAALAQwheR8f//sDE6NJGUBgCRtVIAkZ/CKOvKEABUqQZAuQp6dLgpAgA0"
    + "igIANCsAALBrwRuRqsJfuGt5dLjkAxGqXwELa+EBAFQrAACwa8EfkaoCQLlreXS45AMRql8BC2tg"
    + "/f9UBwAAFOT//7CEtDaRigAANeb//xfk//+whNQ8kSoAAJBLQU65fwkAcQv8/1SN54W5Dt+FuawB"
    + "DsufBQDxa/v/VKrCX7jfAQprz8GKGv8BDWvtsY0arQEOS24FAFGtfUCTq30Om2sFTItrDcyabP2r"
    + "iisAAJBrRU65nwEO64Uxjpp/CQBxK/n/VCwAALBt6oW5juGFuawBDsufBQDxa/j/VKgCQLlrBQBR"
    + "3wEIa8/BiBr/AQ1r7bGNGq0BDkutfUCTrX0Lm60FTIusDcyajP2sip8BC+uGMYuaSQEANIn2ftMr"
    + "AACwa8EbkWppKbgqAACwSsEfkUhpKbgoAIBSAgAAFOgDHyrgB0CRAQiAUuL//7BCxDWRAMARkeMD"
    + "FCoIejS49AgAlAgAAVEf/QAxQwEAVCgAAJDiB0CR4wMAKggpTrlCwBGRIQCAUuADCCpoBACUIPL/"
    + "NigAAJAAKU65oAD4N+AIAJQoAACQCQCAEgkpDrkoAACQ4AdA/SkAALAIGU65P2EXOR8FAHFrAQBU"
    + "KQAAsClRDZEEAAAUCAUA8SlRAJGgAABUKgFAuYr//zQgAQD9+v//FygAALAfURc5cQIAlGAA+DYo"
    + "AACwGtEFuTUAAJA0AIBSOgAAsBkB+LaxCACUCABAuR9NAHFgAABUHxUAcUEAAFRU0wW56L9AeTMA"
    + "AJAfBR1yYdv/VOjPQHnoAAA2YCJOueEDH6riAx+qAwGgUugIAJQgH/g2KAAAkBkpTrnZ2/836N9A"
    + "eR8FHXKhFwBUSNsHNvMDH6oEAAAUEwATi38KAPEiAgBUSACAUuADGSrjAx8qAgETy6hTANEBAROL"
    + "2AgAlIAjALSg/v+2iQgAlAgAQLkfEQBx4SIAVEjTRbkI/v80FAEAFKhzANEIJUA5qBo4NqlzANE0"
    + "IcA5VBr4NokeABIqCQASXwUAcaAAAFSqAYBSKQEKCj8hAHFBGQBUGRlAkj/7AXEBAwBUKAAAkPMD"
    + "H6oZKU65BAAAFBMAE4t/CgDxQgUAVEgAgFLgAxkq4wMfKgIBE8uocwDRAQETi7AIAJSAHgC0oP7/"
    + "tmEIAJQIAEC5HxEAceEdAFRI00W5CP7/NOwAABQ//wFxIQYAVCgAAJDzAx+qGSlOuQQAABQTABOL"
    + "fyIA8aICAFQIAYBS4AMZKuMDHyoCARPLqHMA0QEBE4uXCACUYBsAtKD+/7ZICACUCABAuR8RAHHB"
    + "GgBUSNNFuQj+/zTTAAAUqHMA0SkAgFIIAUA5EQAAFKxzANGIAUA5iRFAOYoFQDmLDUA5CD0QUym9"
    + "cNMIIQoqiglAOSlhC6oIAQoqilFAeCiBCKpJBcBaKAEIqukAgFKqcwDRSWlpODkhCKo/B0DxaBYA"
    + "VJMOABJ/IgBxYwAAVD/7AfHCFQBUKAAAkPUDH6oUKU65BAAAFBUAFYu/EgDxAgIAVIgAgFLBAhWL"
    + "4AMUKgIBFcvjAx8qYwgAlOAUALTA/v+2FAgAlAgAQLkfEQBxQRQAVEjTRbko/v80nwAAFCgAAJDh"
    + "B0CR4gMZqgApTrkhwBGRvgUAlAAT+DfsB0CRq4MA0YzBEZFZAQC06AMfqgkFQJKKaWg4aWlpOEkB"
    + "CUqJaSg4CAUAkT8DCOsh//9UtoMA0X8mAHGAAgBUfyoAccAEAFR/IgBxwBMAVBkFALTqB0CR6AMf"
    + "qgsEgFJKwRGRBAAAFAgFAJE/AwjrQAIAVElpaDg/NQBxJBlKekH//1RLaSg4+P//FygAAJDiB0CR"
    + "QQGAUgApTrlCwBGR4wMZqisAABQIAADw4P//sAAoPJFwAAAUPwMI8aMBAFQoAACQIQCAUuL//7BC"
    + "zDaRAClOueMBgFJjAwCUNQAAkDMAAJA0AIBSOgAAsA3+/xfgB0CR4QdAkeIDGaoAwAmRIcARkRUI"
    + "AJToB0CR4AdAkeEHQJEIwQmRAMAJkSHAAZEfaTk4mQUAlCgAAJDgB0CRGSlOuQDAAZEMCACU4gdA"
    + "keMDAKrgAxkqQsABkSEAgFJFAwCUNQAAkDMAAJA0AIBSOgAAsOC9/zZAAAAUKAAAkAEBgFLi//+w"
    + "QgQ2kTUAABQoAACQ+QMAKggpTrlIAvg3CAAA8OD//7AAEDaRCF1G+eEDgFIiAIBSAwFA+dMHAJQz"
    + "AACQYCpOuagHAJQIAIASaCoOuSgAALAfURc5KAAAsB9hFznjQwCR4AMZKiEAgFKCAoBSBAKAUvQD"
    + "GSqvBwCU47MAkeADGSrBAIBSIgCAUoQAgFKpBwCU4AMZKmEAAJQIAADwCF1G+cADADUDAUD5KAAA"
    + "kOD//7AAHD+RoQOAUiIAgFIZKQ65sAcAlB0AABQoAACQAQGAUuL//7BCwDaRAClOuUMAgFIDAwCU"
    + "NQAAkDMAAJA0AIBSCAAA8OD//7AAlD+RCF1G+QMBQPlhA4BSIgCAUp0HAJTOAACUpf3/FwMBQPng"
    + "//+wANg5kaEDgFIiAIBSlQcAlOADGSprBwCUNQAAkDMAAJA0AIBSOgAAsLj+/xcoAACQ4gdAkQEB"
    + "gFIAKU65QsARkeMDGariAgCUOgAAsN7//xcoAACwKQCAUgnRBbnAA1/W/Xu+qfMLAPn9AwCRMwAA"
    + "kGAqTrmAAPg3UgcAlAgAgBJoKg65MwAAkGAiTrmAAPg3TAcAlAgAgBJoIg65MwAAkGAeTrkgAfg3"
    + "AbKIUuIDHyqBAKhyTwcAlGAeTrlBBwCUCACAEmgeDrkzAACQYC5OueAA+DdBoIpSRgcAlGAuTrk4"
    + "BwCUCACAEmguDrnzC0D5/XvCqMADX9b9e7yp/F8BqfZXAqn0TwOp/QMAkf8LQNH/gwzR4YMMkSIA"
    + "gFLjAx8q8wMAKvSDDJFiBwCUHwQA8QsEAFS3QYFS9QMfqpYSANG3QaFyCAAAFIECFYvgAxMqIgCA"
    + "UuMDHypWBwCUHwQA8YsCAFQVABWLvxIA8Z9qNTjD/v9UyGp1uB8BF2uAAABUqAYAkQj9TdMI/v+0"
    + "yP+DkqgCCIsfCUCxwwAAVOgjQ7npqIhSiQqkch8BCWsgAQBUAACAEv8LQJH/gwyR9E9DqfZXQqn8"
    + "X0Gp/XvEqMADX9bh//+wIeg3keCDDJHigwqRAxCAUusCAJRA/v834f//sCEUN5HggwyR4oMJkQMI"
    + "gFLkAgCUYP3/N+H//7AhPDiR4IMMkeKDB5EDEIBS3QIAlID8/zfh//+wIXA3keCDDJHiAweRAwSA"
    + "UtYCAJSg+/834f//sCEANJHggwmRJQcAlAD7/zXggweRJAMAlKD6/zTog0N5KWaGUuoLRzkIAQlK"
    + "CAEKKuj5/zXggwqRFQcAlB9gAPFh+f9U6P//sAk+nFLgAwSRAD3DPUl6uHLhgwqRAgOAUukTAbng"
    + "Q4A9/48A+f+zAPk8AwCU4f//sCFsO5HgAwSRggSAUjcDAJTgAwSR4bMFkdgDAJTgswWR4QMGkSgE"
    + "AJRg9v834v//0EIEAJHgAwCR4wMGkQEggFK1BgCUCAQAUR/5A3FI9f9U4gMAKuEDAJHgAxMqfAIA"
    + "lB8AAHHgA59apP//F/17vqnzCwD5/QMAkTMAAJBgKk65gAD4N6AGAJQIAIASaCoOuSgAAJApAACw"
    + "CBlOuT9hFzkfBQBxqwEAVOn//7AgaUb9KQAAsClRDZEEAAAUCAUA8SlRAJGgAABUKgFAuYr//zQg"
    + "AQD9+v//FygAALAfURc5MQAAlIAA+DYoAACwKQCAUgnRBbnzC0D5/XvCqMADX9bgAgC0CABAOagC"
    + "ADT9e7yp9wsA+fZXAqn0TwOp/QMAkfYDAar3AwKq9QMDqvMDAKplBgCU9AMAqh8AALmhYwCR4AMT"
    + "qkIBgFJjBgCUiQJAuakAADQAAIASDgAAFAAAgBLAA1/WqQ9A+egDAKoAAIASKQFAOekAADUfARbr"
    + "qwAAVB8BF+tsAABU4AMfKqgCALn0T0Op9wtA+fZXQqn9e8SowANf1v8DAtH9ewKp/G8DqfpnBKn4"
    + "XwWp9lcGqfRPB6n9gwCRFAAA8IguTrkIKfg3MwAAkGjKRbkfBQBxyxEAVHgAgFKZ/5+S9gMfqhUA"
    + "APC1IjmRlwKAUvgFoHI5B6DyBAAAFNYGAJHfwijrSgQAVMlWF5spEUC5af//NP//AKn4WwMpgC5O"
    + "ueEjAJECA4BSgQYAlMAA+LYjBgCUCABAuR8RAHEA//9UKQEAFB9gAPHhJABU//8AqfkPAPmALk65"
    + "4SMAkQIDgFJzBgCUwAD4thUGAJQIAEC5HxEAcQD//1QbAQAUH2AA8SEjAFRoykW53f//Fx8FAHHr"
    + "CwBUeACAUnkAgFJ6AIBSewCAUvYDH6qXAoBS+AWgcjkHoHK6BqBy2wagctxWF5uJD0C56QkANP//"
    + "AKn4WwMpgC5OueEjAJECA4BSVAYAlMAA+Lb2BQCUCABAuR8RAHEA//9U/AAAFB9gAPFBHwBUiANA"
    + "uf//AKn5IwMpgC5OueEjAJECA4BSRQYAlMAA+LbnBQCUCABAuR8RAHEA//9U7QAAFB9gAPFhHQBU"
    + "yFYXm///AKkIBUC5+iMDKYAuTrnhIwCRAgOAUjUGAJTAAPi21wUAlAgAQLkfEQBxAP//VN0AABQf"
    + "YADxYRsAVMhWF5v//wCpCAlAufsjAymALk654SMAkQIDgFIlBgCUwAD4tscFAJQIAEC5HxEAcQD/"
    + "/1TNAAAUH2AA8WEZAFRoAIBS//8AqegGoHLoDwD5gC5OueEjAJECA4BSFQYAlMAA+La3BQCUCABA"
    + "uR8RAHEA//9UvQAAFB9gAPFhFwBUaMpFudYGAJHfwijrq/X/VBUAAPCpGk65PwUAcasQAFR5AIBS"
    + "egCAUpz/n5L2Ax+qlwKAUjgAAJAYIw2R+QWgcjoHoHI8B6Dy22IXm2gTQLnoAQA0aMpFuf//AKkI"
    + "ARYL+SMDKYAuTrnhIwCRAgOAUvAFAJTgAvi2kgUAlAgAQLkfEQBxAP//VJgAABTIYhebCA1AuUgM"
    + "ADRoykW5//8AqQgBFgv5IwMpgC5OueEjAJECA4BS3wUAlIAC+LaBBQCUCABAuR8RAHEA//9UhwAA"
    + "FB9gAPGhEABU//8AqfwPAPmALk654SMAkQIDgFLRBQCUIAn4tnMFAJQIAEC5HxEAcQD//1R5AAAU"
    + "H2AA8eEOAFRoA0C5//8AqfojAymALk654SMAkQIDgFLCBQCUwAD4tmQFAJQIAEC5HxEAcQD//1Rq"
    + "AAAUH2AA8QENAFTIYhebaQCAUv//AKmpBqByCAVAuekjAymALk654SMAkQIDgFKwBQCUwAD4tlIF"
    + "AJQIAEC5HxEAcQD//1RYAAAUH2AA8cEKAFTIYhebaQCAUv//AKnJBqByCAlAuekjAymALk654SMA"
    + "kQIDgFKeBQCUwAD4tkAFAJQIAEC5HxEAcQD//1RGAAAUH2AA8YEIAFRoAIBS//8AqegGoHLoDwD5"
    + "gC5OueEjAJECA4BSjgUAlMAA+LYwBQCUCABAuR8RAHEA//9UNgAAFB9gAPGBBgBUqRqOudYGAJHf"
    + "AgnrC/H/VGjKRbkfBQBxCwEAVOgDCCoKAADwSlE5kUtFQbirAQA1CAUA8aH//1Q/BQBxawEAVOgD"
    + "CSopAACQKVENkSpFQbiKAAA1CAUA8aH//1QEAAAUKACAUgIAABToAx8qKQCAUv//AKlJKaBy6SMD"
    + "KYAuTrnhIwCRAgOAUmMFAJTAAPi2BQUAlAgAQLkfEQBxAP//VAsAABQfYADxIQEAVGIAAJTiAwAq"
    + "IACAUqEogFJFAACUYAAANXQAAJRAAQA0AACAEvRPR6n2V0ap+F9FqfpnRKn8b0Op/XtCqf8DApHA"
    + "A1/WaMpFuR8FAHELAwBUHwUAcWEAAFTpAx+qCwAAFAl1f5IKAADwSrE5kesDCaprCQDxX8EeuF+F"
    + "Arih//9UPwEI60ABAFSKAoBSCwAA8GshOZEqLaqbCAEJy0pBAJEIBQDxX0UBuMH//1SoGk65HwUA"
    + "cesBAFQfBQBxYQAAVOkDH6oNAAAUCXV/kioAAJBKsQ2R6wMJqmsJAPFfwR64X4UCuKH//1Q/AQjr"
    + "YQAAVOADHyrM//8XigKAUisAAJBrIQ2RKi2qmwgBCcvgAx8qSkEAkQgFAPFfRQG4wf//VMH//xf/"
    + "AwHR/XsCqfRPA6n9gwCRFAAA8P//AKngMwB54TcAeeIfALmALk654SMAkQIDgFIFBQCU8wMAqqAA"
    + "+LamBACUCABAuR8RAHHg/v9Uf2IA8eADn1r0T0Op/XtCqf8DAZHAA1/WKAAAkAjJRbkfBQBx6wAA"
    + "VAkAAPApUTmRKkVBuOoBADUIBQDxof//VAgAAPAIGU65HwUAcesAAFQpAACQKVENkSpFQbiqAAA1"
    + "CAUA8aH//1TgAx8qwANf1iAAgFLAA1/W/wMB0f17Aqn0TwOp/YMAkRQAAPD//wCp/w8A+YAuTrnh"
    + "IwCRAgOAUtYEAJTzAwCqoAD4tncEAJQIAEC5HxEAceD+/1R/YgDx4AOfWvRPQ6n9e0Kp/wMBkcAD"
    + "X9Z/BEDxaQAAVAAAgBLAA1/W/0MB0f17Aan4XwKp9lcDqfRPBKn9QwCR8wMDqvQDAqr1AwAqPyAA"
    + "caMAAFR/9gHxaQAAVAAAgBI3AAAUCBCAUn/2AfEoDAAz6BMAOYgAAFRWAIBS6AMTKgYAABRoCsBa"
    + "lgCAUgh9EFPoDwB5yA+AUvcDH6r4EwCR6BcAOQQAABQXABeL/wIW68IBAFTCAhfLAQMXi+ADFSoD"
    + "AIhSogQAlB8AAPHs/v9UAAP4tj4EAJQIAEC5HxEAcWD+/1QTAAAUkwIAtPYDH6oEAAAUFgAWi98C"
    + "E+viAQBUYgIWy4ECFovgAxUqAwCIUo8EAJQfAADx7P7/VKAA+LYrBACUCABAuR8RAHFg/v9UIACA"
    + "UgIAABTgAx8q9E9EqfZXQ6n4X0Kp/XtBqf9DAZHAA1/WYgMAtP17van2VwGp9E8Cqf0DAJHzAwKq"
    + "9AMBqvUDACr2Ax+qBAAAFBYAFovfAhPrIgIAVGICFsuBAhaL4AMVKgMAiFJtBACUHwAA8ez+/1Sg"
    + "APi2CQQAlAgAQLkfEQBxYP7/VAAAgBIEAAAU4AMfKsADX9bgAx8q9E9CqfZXQan9e8OowANf1v17"
    + "u6n6ZwGp+F8CqfZXA6n0TwSp/QMAkfoDAKrgAwGq9AMDqvMDAqr3AwGqRQQAlEgDQDloBwA0+AMA"
    + "qvn//5A5MziRBAAAFMguQDj6AxaqiAYANOADGqrhAxmqSQQAlAAGALT2AwCqAgAay+ADGqpBB4BS"
    + "RwQAlID+/7QIABrL9QMAqh8BGOsB/v9U4AMaquEDF6riAxiqQgQAlGD9/zWhBgCRPwAW6wICAFTo"
    + "AzWqCAEWixUBFYsEAAAUCAUA8SEEAJEAAQBUKQBAOT+BAHFg//9UPyUAcSD//1Q1BADRAgAAFOED"
    + "FqrIAgHroAEAVBYFAJGoAhaLCPFfOB+BAHEEGUl6oQAAVNYGANHfBgDxIf//VAMAABTfAhTrCQEA"
    + "VAAAgBL0T0Sp9ldDqfhfQqn6Z0Gp/XvFqMADX9bCBgDR4AMTqvsDAJRoAhaL4AMfKh/xHzj0//8X"
    + "/Xu8qfhfAan2VwKp9E8Dqf0DAJEVAEA5NQQANDdAgNIYQIDS8wMAqvT//5CUFjeRNgCAUjcAwvI4"
    + "AMLyv7IAcegAAFToAxUqyCLImh8BGOpgAABUdR5AOPn//xfgAxOq4QMUquIAgFL4AwCU4AAANWge"
    + "QDkfsQBxiAAAVMgiyJofARfqgQEAVLUAADS/sgBxYP3/VHUeQDi1//814AMfKvRPQ6n2V0Kp+F9B"
    + "qf17xKjAA1/WIACAUvr//xcIDED5CA0CiwgMAPkCFAC0/4MG0f17FKn8bxWp+mcWqfhfF6n2Vxip"
    + "9E8Zqf0DBZEJMED52jqYUpubl1LzAwKq9AMBqvUDAKoXgACRGAiAUvkDAJFaTLlye+Oxcg4AABTp"
    + "Ax+qTQANCwwCDAsrAAsL6gEKC8gBCAutMgApqyoBKagSALm/MgD5cwIWy5QCFosTDwC0CAMJy+AC"
    + "CYvhAxSqHwET6xYxk5riAxaqnAMAlKgyQPkJARaLPwEB8akyAPlB/v9U4AJATOgDH6okpAhvRaQI"
    + "bwakCC8HpAhvMKQIL1GkCC9ypAhvYKQIL5M4YW60pBhv1aQQL8akEG/2pBBv56QQL4Q4YS6lpBgv"
    + "FzphbnMetE40phhvEDphLjGmGC/WVjhP4VQ4T4IcpU7EVDhPplY4T+UetE5DphBvBx6xTnAetk5R"
    + "phAvQRyhTgKkEG8ApBAvpBykTuUcpk4DHqNOIRyxToIcok6gHKBO4Q8BreALAK0pAwiLCBEAkSo1"
    + "QLkrIUC5LAlAuS0BQLkfAQTxagEKSosBDUpKAQtKSn2KEypBALmB/v9UqqJBKa0yQCmrCkC56QMf"
    + "qvEDCCrjAwoq4gMNKuEDCyrwAwwqEgAAFAEADwrCASAKQQABKiIzj1JCUKtyA26QEyR7abgxAgEL"
    + "KQUAkWEAAgs/QQHx4wMPKjEAEQsBCIATIgIEC/EDDiqA8v9U4AMQKj9NAPHvAwEq7gMDKvADAipJ"
    + "/f9UP50A8cgAAFThAQ5KInSdUiEAAEoi261y6P//F+EBDiriAQ5K4wEOCiEAAAo/7QDxQgAASiEA"
    + "AypBgIEaQoObGt7//xf0T1mp9ldYqfhfV6n6Z1ap/G9Vqf17VKn/gwaRwANf1v/DAtH9ewip9UsA"
    + "+fRPCqn9AwKRCDBA+QDkAG8JD4BSCgeAUvMDAaoVDED5H+EA8eEDAJH0AwCqSTGJmgoQgFIiAQjL"
    + "4BOAPOATgTzgE4I84BODPOAThDzgE4U84BOGPOAfgD3qAwA5Qv//l6gOwNrhAwCR4AMUqgIBgFLo"
    + "AwD5PP//l4gOQDloAgA5iAZAeWgGADmIAkC5CH0IU2gKADmIAkC5aA4AOYgeQDloEgA5iA5AeWgW"
    + "ADmIBkC5CH0IU2gaADmIBkC5aB4AOYguQDloIgA5iBZAeWgmADmICkC5CH0IU2gqADmICkC5aC4A"
    + "OYg+QDloMgA5iB5AeWg2ADmIDkC5CH0IU2g6ADmIDkC5aD4AOYhOQDloQgA5iCZAeWhGADmIEkC5"
    + "CH0IU2hKADmIEkC5aE4AOfRPSqn1S0D5/XtIqf/DApHAA1/W6wMfqugDH6oJCACR6v//kEpdOpEI"
    + "AAAUrAeAUuwJADktAAiLf1EA8QgRAJGsDQA54gQAVOwDC6p/TQDxDwALi2EAAFTwAx8qCAAAFPAF"
    + "QDmfRQDxqAAAVC1pbDiLDQCRLgCAUgQAABTtAx8q7gMfKosCgFLvAUA5EF4YU59NAPHx/ULTAkIP"
    + "Ki8ACItRaXE4QkRM0/EBADlRaWI48QUAOcD7/1QMAg0qjC1G00xpbDjsCQA5jgAANKwVQJJMaWw4"
    + "2P//F6wHgFLW//8X4AMIKj9oKDjAA1/WwgMAtP17vKn3CwD59lcCqfRPA6n9AwCR8wMCqvQDAar1"
    + "AwAq9gMfqhcAAPAEAAAUFgAWi98CE+tCAgBUYgIWy4ECFovgAxUq4wMfKoMCAJQAAQC04P7/tjQC"
    + "AJQIAEC5HxEAcWEAAFTo0kW5SP7/NAAAgBIEAAAU4AMfKsADX9bgAx8q9E9DqfcLQPn2V0Kp/XvE"
    + "qMADX9b/gwHR/XsCqfcbAPn2VwSp9E8Fqf2DAJHzAwGqwf//8CEwOJH0AwCqigIAlJ9qIDjB///w"
    + "Icg3kaJjAJHgAxSqiAIAlOAHALTB///wIVw3kfQDAKoLAgCUIAgANMH///AhnD6R4AMUqgYCAJQg"
    + "CAA0wf//8CFoOJHgAxSqAQIAlKAJADTB///wISg0keADFKr8AQCUAAwANMH///AhrD6R4AMUqvcB"
    + "AJTgCwA0wf//8CG0NpHgAxSq8gEAlEAVADTB///wIdQ8keADFKrtAQCUAAwANMH///AhODSR4AMU"
    + "qugBAJRgCwA0wf//8CHoPJHgAxSq4wEAlOAXADTB///wIZw2keADFKreAQCUQBsANMH///AhxD6R"
    + "4AMUqtkBAJRgJQA0yf//8CntPpHo7Y5SKQFA+cgNoHJoCgC5BgAAFCgPgFLJ///wKZE1kSkBQPlo"
    + "EgB5aQIA+TgAABQI7o1SfxIAOcjtrHJoAgC5MwAAFAgAANAKAADwSnEXkQNBTrkIAADwCJEXkQkA"
    + "ANDC///wQoA4kUUdQCkGIUApJEVOueADE6oBQIBS6AMAuc0BAJQiAAAUCAAA0AgZTrkfBQBxqwEA"
    + "VMn///AgaUb9CQAA8ClRDZEEAAAUCAUA8SlRAJGgAABUKgFAuYr//zQgAQD9+v//FwgAAPAfURc5"
    + "Xfv/l2AB+DYIAADwKQCAUgnRBbkHAAAUCAAA8CkAgFIJYRc5AwAAFAgAAPAfYRc56G2NUn8KADlo"
    + "AgB59E9FqfcbQPn2V0Sp/XtCqf+DAZHAA1/W1f//8LXKN5GiYwCR4AMfquEDFar/AQCU9wMAqqJj"
    + "AJHgAx+q4QMVqvoBAJT2AwCqomMAkeADH6rhAxWq9QEAlAgAAPAIUVc5HwUAcQAZAFT3GAC01hgA"
    + "tPUDAKqAGAC0wf//8CHIN5GiYwCR4AMfqugBAJTAFwC1CAAA0KMTANHgAxeqCBmOueEDH6oCBQDR"
    + "+/r/l8AWADUIAADQ40MAkeADFqoIQY654QMfqgIFANHz+v+XwBUANQgAANDjMwCR4AMVqghFjrnh"
    + "Ax+qAgUA0ev6/5fAFAA14BNAuaIjANHhAx8qEAEAlCAUADXgD0C5ojMA0SEAgFILAQCUgBMANaMH"
    + "fymkQ1+4AAAA8AAgDZEfAAAUwf//8CHIN5GiYwCR4AMfqrsBAJQIAADwCFFXOegRADf1AwCqoBEA"
    + "tMH///AhyDeRomMAkeADH6qxAQCU4BAAtQgAANCjEwDR4AMVqggZjrnhAx+qAgUA0cT6/5fgDwA1"
    + "ocOfuIgCgFIAAADwACANkSgAKJsDkUAp4gMUqpoAAJTADgA14vr/lwDy/zZzAAAUFAAA8IhSVznI"
    + "DwA3wf//8CHIN5GiYwCR4AMfqpQBAJQADwC1AAAA8ADAI5EBAADwISANkQJQgFJpAQCUCAAA0ADk"
    + "AG8pAIBSCDFOuYlSFzkJAADwKHELuQgAAPAI0S2RAAEArQABAa0AAQKtAAEDrehtjVJoAgB5fwoA"
    + "OXL//xfV///wtco3kaJjAJHgAx+q4QMVqnYBAJT3AwCqomMAkeADH6rhAxWqcQEAlPQDAKqiYwCR"
    + "4AMfquEDFapsAQCU9gMAqqJjAJHgAx+q4QMVqmcBAJQIAADwCFFXOR8FAHFBBwBUNwcAtBQHALT2"
    + "BgC09QMAqqAGALTB///wIcg3kaJjAJHgAx+qWQEAlOAFALUIAADQoxMA0eADF6oIGY654QMfqgIF"
    + "ANFs+v+X4AQANQgAANDjQwCR4AMWqghBjrnhAx+qAgUA0WT6/5fgAwA1CAAA0OMzAJHgAxWqCEWO"
    + "ueEDH6oCBQDRXPr/l+ACADXgE0C5oiMA0eEDHyqBAACUQAIANeAPQLmiMwDRIQCAUnwAAJSgAQA1"
    + "tcOfuBYAAPDW0i2RyHp1uAgBADWkj34pAAAA8ADAI5HhAxUq4gMUqiUAAJQgBAA0iA6AUsn///Ap"
    + "NTeR4f7/FxQAAPCIUlc5HwUAceEAAFTB///wIcg3kaJjAJHgAx+qHQEAlKAAALSoDIBSyf//8CkB"
    + "PJHT/v8XAAAA8AAgDZEBAADwIcAjkQJQgFLuAACUCAAA8AkAANAIcUu5KDEOuVH6/5efUhc5wN//"
    + "Nu///xcoAIBSyHo1uPr+/xf9e7yp9wsA+fZXAqn0TwOp/QMAkfYDASr1AwCqwf//8CHUPJHgAwKq"
    + "8wMEKvQDAyr3AwKqgQAAlMACADTB///wITg0keADF6p8AACUAAUANMH///AhtDaR4AMXqncAAJSg"
    + "BQA1iAKAUslWKJsozUC4KAUANMh+QJOKAoBSPwEAuR8gA9UKVSqbSUEAkRUAABSIAoBSylYom+kD"
    + "CqoozUC4qAMANch+QJOLAoBSC1Urm2sRQLkLAwA1CwAA0O3/n1JsMU65jgUAEZ8BDWtuMQ65TAEA"
    + "uWsAAFQqAIBSajEOuSoAgFIqAQC5BgAAFIgCgFLIViibCA1AuegAADTIfkCTiQKAUuADHyoIVSmb"
    + "FM0AKQIAABQAAIAS9E9DqfcLQPn2V0Kp/XvEqMADX9YIAADQCBE5kT8AAHEfIAPVaS8BECgBiJoJ"
    + "AUC56AMAKj8BAGsAAIAS7QIAVMgC+Dc/CQBxiwIAVCp8ftMLAADwa5EXkQwAAPCMcReR6AMIKmtp"
    + "qriKaaq4KQUAUS19AVPgAx8qbAEKy4g1CJsIDcmaCP2oiggBCosfAQvrCLGLmkgAALnAA1/WAAAA"
    + "APB7v6kQAACwEWpG+RBCM5EgAh/WHyAD1R8gA9UfIAPVEAAAsBFuRvkQYjORIAIf1hAAALARckb5"
    + "EIIzkSACH9YQAACwEXZG+RCiM5EgAh/WEAAAsBF6RvkQwjORIAIf1hAAALARfkb5EOIzkSACH9YQ"
    + "AACwEYJG+RACNJEgAh/WEAAAsBGGRvkQIjSRIAIf1hAAALARikb5EEI0kSACH9YQAACwEY5G+RBi"
    + "NJEgAh/WEAAAsBGSRvkQgjSRIAIf1hAAALARlkb5EKI0kSACH9YQAACwEZpG+RDCNJEgAh/WEAAA"
    + "sBGeRvkQ4jSRIAIf1hAAALARokb5EAI1kSACH9YQAACwEaZG+RAiNZEgAh/WEAAAsBGqRvkQQjWR"
    + "IAIf1hAAALARrkb5EGI1kSACH9YQAACwEbJG+RCCNZEgAh/WEAAAsBG2RvkQojWRIAIf1hAAALAR"
    + "ukb5EMI1kSACH9YQAACwEb5G+RDiNZEgAh/WEAAAsBHCRvkQAjaRIAIf1hAAALARxkb5ECI2kSAC"
    + "H9YQAACwEcpG+RBCNpEgAh/WEAAAsBHORvkQYjaRIAIf1hAAALAR0kb5EII2kSACH9YQAACwEdZG"
    + "+RCiNpEgAh/WEAAAsBHaRvkQwjaRIAIf1hAAALAR3kb5EOI2kSACH9YQAACwEeJG+RACN5EgAh/W"
    + "EAAAsBHmRvkQIjeRIAIf1hAAALAR6kb5EEI3kSACH9YQAACwEe5G+RBiN5EgAh/WEAAAsBHyRvkQ"
    + "gjeRIAIf1hAAALAR9kb5EKI3kSACH9YQAACwEfpG+RDCN5EgAh/WEAAAsBH+RvkQ4jeRIAIf1gEA"
    + "AAAAAAAAQAEAAAAAAAABAAAAAAAAADMBAAAAAAAAHgAAAAAAAAAIAAAAAAAAAPv//28AAAAAAQAA"
    + "CAAAAAAVAAAAAAAAAAAAAAAAAAAABwAAAAAAAADYCAAAAAAAAAgAAAAAAAAAeAAAAAAAAAAJAAAA"
    + "AAAAABgAAAAAAAAA+f//bwAAAAAEAAAAAAAAABcAAAAAAAAAUAkAAAAAAAACAAAAAAAAAHgDAAAA"
    + "AAAAAwAAAAAAAADAbAAAAAAAABQAAAAAAAAABwAAAAAAAAAGAAAAAAAAAFgDAAAAAAAACwAAAAAA"
    + "AAAYAAAAAAAAAAUAAAAAAAAAjAcAAAAAAAAKAAAAAAAAAEkBAAAAAAAA9f7/bwAAAABwBwAAAAAA"
    + "APD//28AAAAAAAcAAAAAAAD+//9vAAAAAFAHAAAAAAAA////bwAAAAABAAAAAAAAAAAAAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    + "AAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBY"
    + "AAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgA"
    + "AAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAA"
    + "AAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAA"
    + "AAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAAAADQWAAAAAAAANBYAAAAAAAA0FgAAAAA"
    + "AADQWAAAAAAAANBYAAAAAAAA//////////8AAAAAAAAAAAAAAAAAAAAACgAAAP//////////L2oA"
    + "AP//////////AQAAAABBbmRyb2lkICgxMzY5MTU1NywgK3BnbywgK2JvbHQsICtsdG8sICttbGdv"
    + "LCBiYXNlZCBvbiByNTIyODE3ZCkgY2xhbmcgdmVyc2lvbiAxOC4wLjQgKGh0dHBzOi8vYW5kcm9p"
    + "ZC5nb29nbGVzb3VyY2UuY29tL3Rvb2xjaGFpbi9sbHZtLXByb2plY3QgZDgwMDNhNDU2ZDE0YTNk"
    + "ZWI4MDU0Y2RhYTUyOWZmYmYwMmQ5YjI2MikAQW5kcm9pZCAoMTM2OTE1NTcsIGJhc2VkIG9uIHI1"
    + "MjI4MTdkKSBjbGFuZyB2ZXJzaW9uIDE4LjAuNCAoaHR0cHM6Ly9hbmRyb2lkLmdvb2dsZXNvdXJj"
    + "ZS5jb20vdG9vbGNoYWluL2xsdm0tcHJvamVjdCBkODAwM2E0NTZkMTRhM2RlYjgwNTRjZGFhNTI5"
    + "ZmZiZjAyZDliMjYyKQBMaW5rZXI6IExMRCAxOC4wLjQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    + "AAABAAAABADx/wAAAAAAAAAAAAAAAAAAAAAMAAAAAAANAMgmAAAAAAAAAAAAAAAAAAARAAAAAgAN"
    + "ANwmAAAAAAAAWAAAAAAAAAAdAAAAAQATAAB+AAAAAAAAGAAAAAAAAAA3AAAAAgANAHQnAAAAAAAA"
    + "VAAAAAAAAABHAAAAAAATAAB+AAAAAAAAAAAAAAAAAABMAAAAAAAUADh+AAAAAAAAAAAAAAAAAABR"
    + "AAAAAAAVAAAAAAAAAAAAAAAAAAAAAABWAAAAAAAMAMgRAAAAAAAAAAAAAAAAAABbAAAAAQACAMAC"
    + "AAAAAAAAmAAAAAAAAABuAAAAAAACAMACAAAAAAAAAAAAAAAAAABzAAAAAAACANQCAAAAAAAAAAAA"
    + "AAAAAAB9AAAAAAACAMwCAAAAAAAAAAAAAAAAAACHAAAAAAACAFgDAAAAAAAAAAAAAAAAAACQAAAA"
    + "AAACANgCAAAAAAAAAAAAAAAAAACcAAAAAAACABgDAAAAAAAAAAAAAAAAAAD5AgAAAgINADQnAAAA"
    + "AAAAFAAAAAAAAAASAwAAAgINAEgnAAAAAAAAHAAAAAAAAAAZAwAAAQIUADh+AAAAAAAACAAAAAAA"
    + "AAAzAwAAAgINAGQnAAAAAAAAEAAAAAAAAACtAAAABADx/wAAAAAAAAAAAAAAAAAAAAC3AAAAAAAK"
    + "ANAMAAAAAAAAAAAAAAAAAAC8AAAAAAANAMgnAAAAAAAAAAAAAAAAAADBAAAAAgANADA8AAAAAAAA"
    + "EAAAAAAAAADLAAAAAQAUAEB+AAAAAAAABAAAAAAAAADZAAAAAQATABh+AAAAAAAABAAAAAAAAADg"
    + "AAAAAQAUAER+AAAAAAAABAAAAAAAAADvAAAAAQATACR+AAAAAAAABAAAAAAAAAD3AAAAAQAUAEh+"
    + "AAAAAAAAAAUAAAAAAAD8AAAAAQAUAEiDAAAAAAAAgAIAAAAAAAABAQAAAQAUAMiFAAAAAAAABAAA"
    + "AAAAAAAMAQAAAQAUANyFAAAAAAAACAAAAAAAAAASAQAAAQAUAOSFAAAAAAAACAAAAAAAAAAYAQAA"
    + "AQAUAMyFAAAAAAAABAAAAAAAAAAkAQAAAQATACx+AAAAAAAABAAAAAAAAAApAQAAAQATABx+AAAA"
    + "AAAABAAAAAAAAAAyAQAAAQATACB+AAAAAAAABAAAAAAAAAA8AQAAAgANAEA8AAAAAAAAmAAAAAAA"
    + "AABEAQAAAQAUANCFAAAAAAAABAAAAAAAAABOAQAAAQATACh+AAAAAAAABAAAAAAAAABYAQAAAQAU"
    + "AOyFAAAAAAAABAAAAAAAAABmAQAAAgANAEBAAAAAAAAATAYAAAAAAABxAQAAAQAUANiFAAAAAAAA"
    + "AQAAAAAAAAB8AQAAAQAUAPCFAAAAAAAAAAEAAAAAAACEAQAAAQAUAPCGAAAAAAAAAAEAAAAAAACJ"
    + "AQAAAQAUAPCHAAAAAAAAAAEAAAAAAACOAQAAAgANAKxHAAAAAAAAPAEAAAAAAACWAQAAAQAUANSF"
    + "AAAAAAAAAQAAAAAAAAChAQAAAgANADhQAAAAAAAAmAAAAAAAAACrAQAAAgANANBQAAAAAAAASAYA"
    + "AAAAAAC3AQAAAgANANg8AAAAAAAAMAIAAAAAAADLAQAAAgANAAg/AAAAAAAAlAAAAAAAAADXAQAA"
    + "AAAKAPAMAAAAAAAAAAAAAAAAAADcAQAAAgANAHBJAAAAAAAAWAEAAAAAAADpAQAAAgANAMhKAAAA"
    + "AAAAvAAAAAAAAADzAQAAAgANAIRLAAAAAAAAkAIAAAAAAAD/AQAAAgANABROAAAAAAAATAEAAAAA"
    + "AAAKAgAAAgANAGBPAAAAAAAA2AAAAAAAAAARAgAAAgANAOhIAAAAAAAAiAAAAAAAAAAcAgAAAgAN"
    + "AJw/AAAAAAAApAAAAAAAAAAnAgAAAgANAPBGAAAAAAAAYAAAAAAAAAAwAgAAAgANAIxGAAAAAAAA"
    + "ZAAAAAAAAAA1AgAAAgANAFBHAAAAAAAAXAAAAAAAAAA5AgAAAQAKAJcOAAAAAAAAQQAAAAAAAABE"
    + "AgAAAgANAERYAAAAAAAAiAAAAAAAAABTAgAAAgANABhXAAAAAAAALAEAAAAAAABfAgAAAQAUAPCI"
    + "AAAAAAAAgAIAAAAAAABmAgAAAQATADB+AAAAAAAABAAAAAAAAAB3AgAAAQAUAHCLAAAAAAAABAAA"
    + "AAAAAACBAgAAAQAUAHSLAAAAAAAAgAAAAAAAAACMAgAAAAAUAEB+AAAAAAAAAAAAAAAAAACRAgAA"
    + "AAAKAB0NAAAAAAAAAAAAAAAAAACWAgAAAAATABh+AAAAAAAAAAAAAAAAAACbAgAAAAAKAJwQAAAA"
    + "AAAAAAAAAAAAAACgAgAAAAAVAAAAAAAAAAAAAAAAAAAAAAClAgAAAAAMAMgRAAAAAAAAAAAAAAAA"
    + "AACqAgAAAQAMAMgRAAAAAAAABAAAAAAAAAC4AgAAAAAMAMgRAAAAAAAAAAAAAAAAAADEAgAAAAIN"
    + "AMgmAAAAAAAAAAAAAAAAAADXAgAAAAINAMgmAAAAAAAAAAAAAAAAAABbBAAAAAIPAEBrAAAAAAAA"
    + "AAAAAAAAAAC9AgAAEgANAMgmAAAAAAAAFAAAAAAAAADoAgAAEgANAMgnAAAAAAAAaBQAAAAAAADt"
    + "AgAAEgAAAAAAAAAAAAAAAAAAAAAAAAAmAwAAEgAAAAAAAAAAAAAAAAAAAAAAAABCAwAAEgAAAAAA"
    + "AAAAAAAAAAAAAAAAAABUAwAAEgAAAAAAAAAAAAAAAAAAAAAAAABgAwAAEgAAAAAAAAAAAAAAAAAA"
    + "AAAAAABqAwAAEgAAAAAAAAAAAAAAAAAAAAAAAABxAwAAEgAAAAAAAAAAAAAAAAAAAAAAAAB4AwAA"
    + "EgAAAAAAAAAAAAAAAAAAAAAAAACAAwAAEgAAAAAAAAAAAAAAAAAAAAAAAACHAwAAEQAAAAAAAAAA"
    + "AAAAAAAAAAAAAACOAwAAEgAAAAAAAAAAAAAAAAAAAAAAAACWAwAAEgAAAAAAAAAAAAAAAAAAAAAA"
    + "AACdAwAAEgAAAAAAAAAAAAAAAAAAAAAAAACjAwAAEgAAAAAAAAAAAAAAAAAAAAAAAACsAwAAEgAA"
    + "AAAAAAAAAAAAAAAAAAAAAACxAwAAEgAAAAAAAAAAAAAAAAAAAAAAAAC3AwAAEgAAAAAAAAAAAAAA"
    + "AAAAAAAAAAC+AwAAEgAAAAAAAAAAAAAAAAAAAAAAAADJAwAAEgAAAAAAAAAAAAAAAAAAAAAAAADP"
    + "AwAAEgAAAAAAAAAAAAAAAAAAAAAAAADZAwAAEgAAAAAAAAAAAAAAAAAAAAAAAADeAwAAEgAAAAAA"
    + "AAAAAAAAAAAAAAAAAADlAwAAEgAAAAAAAAAAAAAAAAAAAAAAAADsAwAAEgAAAAAAAAAAAAAAAAAA"
    + "AAAAAAD1AwAAEgAAAAAAAAAAAAAAAAAAAAAAAAD6AwAAEgAAAAAAAAAAAAAAAAAAAAAAAAD/AwAA"
    + "EgAAAAAAAAAAAAAAAAAAAAAAAAAHBAAAEgAAAAAAAAAAAAAAAAAAAAAAAAAMBAAAEgAAAAAAAAAA"
    + "AAAAAAAAAAAAAAATBAAAEgAAAAAAAAAAAAAAAAAAAAAAAAAaBAAAEgAAAAAAAAAAAAAAAAAAAAAA"
    + "AAAlBAAAEgAAAAAAAAAAAAAAAAAAAAAAAAArBAAAEgAAAAAAAAAAAAAAAAAAAAAAAAAwBAAAEgAA"
    + "AAAAAAAAAAAAAAAAAAAAAAA3BAAAEgAAAAAAAAAAAAAAAAAAAAAAAAA+BAAAEgAAAAAAAAAAAAAA"
    + "AAAAAAAAAABKBAAAEgAAAAAAAAAAAAAAAAAAAAAAAABSBAAAEgAAAAAAAAAAAAAAAAAAAAAAAAAA"
    + "LmludGVycAAubm90ZS5hbmRyb2lkLmlkZW50AC5keW5zeW0ALmdudS52ZXJzaW9uAC5nbnUudmVy"
    + "c2lvbl9yAC5nbnUuaGFzaAAuZHluc3RyAC5yZWxhLmR5bgAucmVsYS5wbHQALnJvZGF0YQAuZWhf"
    + "ZnJhbWVfaGRyAC5laF9mcmFtZQAudGV4dAAucGx0AC5keW5hbWljAC5nb3QALmdvdC5wbHQALnJl"
    + "bHJvX3BhZGRpbmcALmRhdGEALmJzcwAuY29tbWVudAAuc3ltdGFiAC5zaHN0cnRhYgAuc3RydGFi"
    + "AABjcnRiZWdpbi5jACR4LjEAX3N0YXJ0X21haW4AZmluaV9hcnJheV93aXRoX3NlbnRpbmVscwBj"
    + "YWxsX2ZpbmlfYXJyYXkAJGQuMgAkZC4zACRkLjQAJGQuNQBub3RlX2FuZHJvaWRfaWRlbnQAJGQu"
    + "MABub3RlX2RhdGEAbm90ZV9uYW1lAG5vdGVfZW5kAG5ka192ZXJzaW9uAG5ka19idWlsZF9udW1i"
    + "ZXIAdnRvdWNoZC5jACRkLjAAJHguMQBvbl9zaWduYWwAbG9naWNhbF93aWR0aAB2c2xvdHMAbG9n"
    + "aWNhbF9oZWlnaHQAd3NfcG9ydABwaHlzAHZpcnQAcGh5c19zbG90cwBheG1pbgBheG1heAB0b3Rh"
    + "bF9zbG90cwB1X2ZkAGlucHV0X2ZkAGxpc3Rlbl9mZABjbGVhbnVwAHN0b3BfZmxhZwBjbGllbnRf"
    + "ZmQAc2VsZWN0ZWRfc2xvdABlbWl0X2ZyYW1lAHN1YnNjcmliZWQAcHNfZG93bgBwc194AHBzX3kA"
    + "d3Nfc2VuZABmcmFtZV9vcGVuAHJlYWRfZnVsbABoYW5kbGVfbGluZQB3ZWJzb2NrZXRfaGFuZHNo"
    + "YWtlAGRyb3BfY2xpZW50ACRkLjIAaGVhZGVyX3ZhbHVlAGhhc190b2tlbgBzaGExX3VwZGF0ZQBz"
    + "aGExX2ZpbmFsAGJhc2U2NAB3cml0ZV9mdWxsAHBhcnNlX2xvbmcAYW55X2Rvd24AZW1pdABzeW4A"
    + "YmFzZTY0LnRhYgBsb2dpY2FsX3RvX3JhdwBzZXRfdmlydHVhbABzdGFnZWQAbmV4dF90cmFja2lu"
    + "Z19pZABzdGFnZWRfaWQAZnJhbWVfc2VlbgAkZC4zACRkLjQAJGQuNQAkZC42ACRkLjcAJGQuOABf"
    + "X0ZSQU1FX0VORF9fACRkLjEAX3N0YXJ0AF9fZmluaV9hcnJheV9zdGFydABfX2ZpbmlfYXJyYXlf"
    + "ZW5kAG1haW4AX19saWJjX2luaXQAX19hdGV4aXRfaGFuZGxlcl93cmFwcGVyAGF0ZXhpdABfX2Rz"
    + "b19oYW5kbGUAX19jeGFfYXRleGl0AHB0aHJlYWRfYXRmb3JrAF9fcmVnaXN0ZXJfYXRmb3JrAHNp"
    + "Z2VtcHR5c2V0AHNpZ2FjdGlvbgBzaWduYWwAc3RyY21wAF9fZXJybm8Ac3RydG9sAHN0ZGVycgBm"
    + "cHJpbnRmAG1lbXNldABjbG9zZQBzbnByaW50ZgBvcGVuAGlvY3RsAHNvY2tldABzZXRzb2Nrb3B0"
    + "AGZjbnRsAGluZXRfcHRvbgBiaW5kAGxpc3RlbgBmd3JpdGUAc3RyZXJyb3IAcG9sbAByZWFkAGFj"
    + "Y2VwdDQAcmVjdgBtZW1jcHkAc3RybGVuAHN0cmNhc2VjbXAAd3JpdGUAc2VuZABzdHJzdHIAbWVt"
    + "Y2hyAHN0cm5jYXNlY21wAHN0cmNzcG4Ac3RydG9rX3IAX0RZTkFNSUMAAAAAAAAAAAAAAAAAAAAA"
    + "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEA"
    + "AAABAAAAAgAAAAAAAACoAgAAAAAAAKgCAAAAAAAAFQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
    + "AAAAAAAJAAAABwAAAAIAAAAAAAAAwAIAAAAAAADAAgAAAAAAAJgAAAAAAAAAAAAAAAAAAAAEAAAA"
    + "AAAAAAAAAAAAAAAAHQAAAAsAAAACAAAAAAAAAFgDAAAAAAAAWAMAAAAAAACoAwAAAAAAAAcAAAAB"
    + "AAAACAAAAAAAAAAYAAAAAAAAACUAAAD///9vAgAAAAAAAAAABwAAAAAAAAAHAAAAAAAATgAAAAAA"
    + "AAADAAAAAAAAAAIAAAAAAAAAAgAAAAAAAAAyAAAA/v//bwIAAAAAAAAAUAcAAAAAAABQBwAAAAAA"
    + "ACAAAAAAAAAABwAAAAEAAAAEAAAAAAAAAAAAAAAAAAAAQQAAAPb//28CAAAAAAAAAHAHAAAAAAAA"
    + "cAcAAAAAAAAcAAAAAAAAAAMAAAAAAAAACAAAAAAAAAAAAAAAAAAAAEsAAAADAAAAAgAAAAAAAACM"
    + "BwAAAAAAAIwHAAAAAAAASQEAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAABTAAAABAAAAAIA"
    + "AAAAAAAA2AgAAAAAAADYCAAAAAAAAHgAAAAAAAAAAwAAAAAAAAAIAAAAAAAAABgAAAAAAAAAXQAA"
    + "AAQAAABCAAAAAAAAAFAJAAAAAAAAUAkAAAAAAAB4AwAAAAAAAAMAAAARAAAACAAAAAAAAAAYAAAA"
    + "AAAAAGcAAAABAAAAMgAAAAAAAADQDAAAAAAAANAMAAAAAAAAHAQAAAAAAAAAAAAAAAAAABAAAAAA"
    + "AAAAAAAAAAAAAABvAAAAAQAAAAIAAAAAAAAA7BAAAAAAAADsEAAAAAAAANwAAAAAAAAAAAAAAAAA"
    + "AAAEAAAAAAAAAAAAAAAAAAAAfQAAAAEAAAACAAAAAAAAAMgRAAAAAAAAyBEAAAAAAAAABQAAAAAA"
    + "AAAAAAAAAAAACAAAAAAAAAAAAAAAAAAAAIcAAAABAAAABgAAAAAAAADIJgAAAAAAAMgWAAAAAAAA"
    + "BDIAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAACNAAAAAQAAAAYAAAAAAAAA0FgAAAAAAADQ"
    + "SAAAAAAAAHACAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAkgAAAAYAAAADAAAAAAAAAEBr"
    + "AAAAAAAAQEsAAAAAAABgAQAAAAAAAAcAAAAAAAAACAAAAAAAAAAQAAAAAAAAAJsAAAABAAAAAwAA"
    + "AAAAAACgbAAAAAAAAKBMAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAgAAAAAAAAAAAAAAAAAAACgAAAA"
    + "AQAAAAMAAAAAAAAAwGwAAAAAAADATAAAAAAAAEABAAAAAAAAAAAAAAAAAAAIAAAAAAAAAAAAAAAA"
    + "AAAAqQAAAAgAAAADAAAAAAAAAABuAAAAAAAAAE4AAAAAAAAAAgAAAAAAAAAAAAAAAAAAAQAAAAAA"
    + "AAAAAAAAAAAAALgAAAABAAAAAwAAAAAAAAAAfgAAAAAAAABOAAAAAAAANAAAAAAAAAAAAAAAAAAA"
    + "AAgAAAAAAAAAAAAAAAAAAAC+AAAACAAAAAMAAAAAAAAAOH4AAAAAAAA0TgAAAAAAALwNAAAAAAAA"
    + "AAAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAwwAAAAEAAAAwAAAAAAAAAAAAAAAAAAAANE4AAAAAAABq"
    + "AQAAAAAAAAAAAAAAAAAAAQAAAAAAAAABAAAAAAAAAMwAAAACAAAAAAAAAAAAAAAAAAAAAAAAAKBP"
    + "AAAAAAAAcAsAAAAAAAAYAAAAUgAAAAgAAAAAAAAAGAAAAAAAAADUAAAAAwAAAAAAAAAAAAAAAAAA"
    + "AAAAAAAQWwAAAAAAAOYAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAA3gAAAAMAAAAAAAAA"
    + "AAAAAAAAAAAAAAAA9lsAAAAAAABkBAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAA==";

/* ---- UI/overlay 源码（调用侧 eval(vt.uiSource) 进主上下文执行） ---- */
var VTOUCH_UI_SRC = "/* 区域 overlay + 管理 UI + watcher 启动器：调用侧 eval(vt.uiSource) 进主上下文执行。\n * 原因：Java bridge 回调（线程/事件/点击/画布）不能定义在 require 模块里。\n * 只用 vt.loadRegions / vt.rgSave / vt.ensure / vt.connect / vt.sub / vt.createEngine / vt.parseEv / vt.finger / vt.stop。\n * 调用侧只剩：eval + bootWatch(handlers) + 业务。管理：ui() 开 / uiClose() 关。 */\nvar g_ovW = null, g_ovR = [], g_ovF = [], g_ovH = {}, g_ovLoc = null;\nfunction ovShow(rs) {\n    g_ovR = rs;\n    if (g_ovW) return;\n    g_ovW = floaty.rawWindow('<frame><canvas id=\"board\" layout_weight=\"1\"/></frame>');\n    g_ovW.setSize(device.width, device.height);\n    g_ovW.setTouchable(false);\n    g_ovW.board.on(\"draw\", function (canvas) {\n        var i, r, p, lx, ly;\n        /* 线程池 draw（AutoJs6 6.x ScriptCanvasView mDrawingThreadPool）与 ovUpdate/ovSet/ovClose 并发：\n         * 先快照全局引用，防执行中途被置 null/换数组。 */\n        var w = g_ovW, rs = g_ovR, fs = g_ovF;\n        if (!w) return;\n        try { canvas.drawColor(colors.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR); } catch (e) {}\n        var oy = 0, ox = 0;\n        try {\n            if (!g_ovLoc) g_ovLoc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);\n            w.board.getLocationOnScreen(g_ovLoc);\n            ox = g_ovLoc[0]; oy = g_ovLoc[1];\n        } catch (e) {}\n        for (i = 0; i < rs.length; i++) {\n            r = rs[i];\n            if (r.hidden) continue;\n            p = new Paint(); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(3); p.setColor(colors.RED);\n            if (g_ovH[r.id] && Date.now() - g_ovH[r.id] < 400) { p.setStrokeWidth(6); p.setColor(colors.GREEN); }\n            if (r.type === \"circle\") { canvas.drawCircle(r.cx - ox, r.cy - oy, r.r, p); lx = r.cx - r.r; ly = r.cy - r.r; }\n            else { canvas.drawRect(r.x1 - ox, r.y1 - oy, r.x2 - ox, r.y2 - oy, p); lx = r.x1; ly = r.y1; }\n            p = new Paint(); p.setColor(colors.WHITE); p.setTextSize(36);\n            canvas.drawText(r.name || r.id, lx - ox + 8, ly - oy + 40, p);\n        }\n        for (i = 0; i < fs.length; i++) {\n            var f = fs[i];\n            if (!f.down) continue;\n            p = new Paint(); p.setColor(colors.BLUE);\n            canvas.drawCircle(f.x - ox, f.y - oy, 40, p);\n            canvas.drawText(\"s\" + f.slot, f.x - ox + 44, f.y - oy, p);\n        }\n    });\n}\nfunction ovUpdate(f) { g_ovF = f || []; try { g_ovW.board.postInvalidate(); } catch (e) {} }\nfunction ovFlash(id) { g_ovH[id] = Date.now(); }\nfunction ovSet(rs) { g_ovR = rs; }\nfunction ovClose() { try { if (g_ovW) g_ovW.close(); } catch (e) {} g_ovW = null; }\nfunction ovPreview(on) { if (on) ovShow(vt.loadRegions()); else ovClose(); }\n/* ---- 管理 UI（深色卡片）：列表查看 | 开关触发 | 显隐预览 | 删除 | ＋矩形/圆形框选 | 预览总开关 ---- */\nvar g_uiW = null, g_capW = null, g_cap = null;\nvar g_uiH = new android.os.Handler(android.os.Looper.getMainLooper());\nfunction rgInfo(r) {\n    var head = (r.enabled === false ? \"[关] \" : \"[开] \") + (r.hidden ? \"[隐] \" : \"\") + (r.name || r.id);\n    if (r.type === \"circle\") return head + \" | O r=\" + r.r + \" @ \" + r.cx + \",\" + r.cy;\n    return head + \" | 口 \" + (r.x2 - r.x1) + \"x\" + (r.y2 - r.y1) + \" @ \" + r.x1 + \",\" + r.y1;\n}\n/* 八槽静态行：XML 里写死，不用程序化 view（API36 ColorOS 上程序化 Button 默认色会毒崩 TextView 绘制）。 */\nfunction uiRowAct(k, what) {\n    var rs = vt.loadRegions();\n    if (k < 0 || k >= rs.length) return;\n    var r = rs[k], i;\n    if (what === \"t\") r.enabled = (r.enabled === false);\n    else if (what === \"v\") r.hidden = !r.hidden;\n    else if (what === \"d\") { var all = []; for (i = 0; i < rs.length; i++) if (i !== k) all.push(rs[i]); rs = all; }\n    vt.rgSave(rs); ovSet(rs); uiRefresh();\n    toast(what === \"d\" ? \"已删除\" : \"已更新 \" + (r.name || r.id));\n}\nfunction uiRefresh() {\n    if (!g_uiW) return;\n    try {\n        g_uiH.post(new JavaAdapter(java.lang.Runnable, { run: function () {\n            try { uiRefreshDo(); } catch (e) { log(\"uiRefresh FAIL \" + e); }\n        } }));\n    } catch (e) { log(\"uiRefresh post FAIL \" + e); }\n}\nfunction uiRefreshDo() {\n    var rs = vt.loadRegions();\n    var extra = rs.length > 8 ? \"（仅显示前8个）\" : \"\";\n    g_uiW.title.setText(\"区域管理 (\" + rs.length + \"个)\" + extra);\n    g_uiW.preview.setText(g_ovW ? \"预览：开\" : \"预览：关\");\n    if (rs.length > 0) { g_uiW.s0t.setText(rgInfo(rs[0])); g_uiW.s0.setVisibility(0); }\n    else { g_uiW.s0.setVisibility(8); }\n    if (rs.length > 1) { g_uiW.s1t.setText(rgInfo(rs[1])); g_uiW.s1.setVisibility(0); }\n    else { g_uiW.s1.setVisibility(8); }\n    if (rs.length > 2) { g_uiW.s2t.setText(rgInfo(rs[2])); g_uiW.s2.setVisibility(0); }\n    else { g_uiW.s2.setVisibility(8); }\n    if (rs.length > 3) { g_uiW.s3t.setText(rgInfo(rs[3])); g_uiW.s3.setVisibility(0); }\n    else { g_uiW.s3.setVisibility(8); }\n    if (rs.length > 4) { g_uiW.s4t.setText(rgInfo(rs[4])); g_uiW.s4.setVisibility(0); }\n    else { g_uiW.s4.setVisibility(8); }\n    if (rs.length > 5) { g_uiW.s5t.setText(rgInfo(rs[5])); g_uiW.s5.setVisibility(0); }\n    else { g_uiW.s5.setVisibility(8); }\n    if (rs.length > 6) { g_uiW.s6t.setText(rgInfo(rs[6])); g_uiW.s6.setVisibility(0); }\n    else { g_uiW.s6.setVisibility(8); }\n    if (rs.length > 7) { g_uiW.s7t.setText(rgInfo(rs[7])); g_uiW.s7.setVisibility(0); }\n    else { g_uiW.s7.setVisibility(8); }\n}\nfunction uiWire(v, fn) {\n    v.setOnClickListener(new JavaAdapter(android.view.View.OnClickListener, { onClick: function () { try { fn(); } catch (e) {} } }));\n}\nfunction ui() {\n    if (g_uiW) { uiClose(); return; }\n    g_uiW = floaty.window(\n        '<vertical bg=\"#1F2430\" padding=\"16\">' +\n        '<text id=\"title\" textSize=\"17sp\" textColor=\"#7FD4FF\" text=\"区域管理\"/>' +\n        '<text textSize=\"12sp\" textColor=\"#9AA4B2\" text=\"开/关=是否触发  显/隐=预览是否绘制\"/>' +\n        '<scroll><vertical id=\"rows\"><vertical id=\"s0\"><text id=\"s0t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s0a\" text=\"开关\"/><button id=\"s0b\" text=\"显隐\"/><button id=\"s0c\" text=\"删\"/></horizontal></vertical><vertical id=\"s1\"><text id=\"s1t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s1a\" text=\"开关\"/><button id=\"s1b\" text=\"显隐\"/><button id=\"s1c\" text=\"删\"/></horizontal></vertical><vertical id=\"s2\"><text id=\"s2t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s2a\" text=\"开关\"/><button id=\"s2b\" text=\"显隐\"/><button id=\"s2c\" text=\"删\"/></horizontal></vertical><vertical id=\"s3\"><text id=\"s3t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s3a\" text=\"开关\"/><button id=\"s3b\" text=\"显隐\"/><button id=\"s3c\" text=\"删\"/></horizontal></vertical><vertical id=\"s4\"><text id=\"s4t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s4a\" text=\"开关\"/><button id=\"s4b\" text=\"显隐\"/><button id=\"s4c\" text=\"删\"/></horizontal></vertical><vertical id=\"s5\"><text id=\"s5t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s5a\" text=\"开关\"/><button id=\"s5b\" text=\"显隐\"/><button id=\"s5c\" text=\"删\"/></horizontal></vertical><vertical id=\"s6\"><text id=\"s6t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s6a\" text=\"开关\"/><button id=\"s6b\" text=\"显隐\"/><button id=\"s6c\" text=\"删\"/></horizontal></vertical><vertical id=\"s7\"><text id=\"s7t\" textSize=\"13sp\" textColor=\"#E8ECF1\" text=\"\"/><horizontal><button id=\"s7a\" text=\"开关\"/><button id=\"s7b\" text=\"显隐\"/><button id=\"s7c\" text=\"删\"/></horizontal></vertical></vertical></scroll>' +\n        '<horizontal><button id=\"addRect\" text=\"＋矩形\"/><button id=\"addCircle\" text=\"＋圆形\"/></horizontal>' +\n        '<horizontal><button id=\"preview\" text=\"预览：关\"/><button id=\"btnQuit\" text=\"关闭\"/></horizontal>' +\n        '</vertical>');\n    log(\"ui window ok\");\n    uiWire(g_uiW.s0a, function () { uiRowAct(0, \"t\"); });\n    uiWire(g_uiW.s0b, function () { uiRowAct(0, \"v\"); });\n    uiWire(g_uiW.s0c, function () { uiRowAct(0, \"d\"); });\n    uiWire(g_uiW.s1a, function () { uiRowAct(1, \"t\"); });\n    uiWire(g_uiW.s1b, function () { uiRowAct(1, \"v\"); });\n    uiWire(g_uiW.s1c, function () { uiRowAct(1, \"d\"); });\n    uiWire(g_uiW.s2a, function () { uiRowAct(2, \"t\"); });\n    uiWire(g_uiW.s2b, function () { uiRowAct(2, \"v\"); });\n    uiWire(g_uiW.s2c, function () { uiRowAct(2, \"d\"); });\n    uiWire(g_uiW.s3a, function () { uiRowAct(3, \"t\"); });\n    uiWire(g_uiW.s3b, function () { uiRowAct(3, \"v\"); });\n    uiWire(g_uiW.s3c, function () { uiRowAct(3, \"d\"); });\n    uiWire(g_uiW.s4a, function () { uiRowAct(4, \"t\"); });\n    uiWire(g_uiW.s4b, function () { uiRowAct(4, \"v\"); });\n    uiWire(g_uiW.s4c, function () { uiRowAct(4, \"d\"); });\n    uiWire(g_uiW.s5a, function () { uiRowAct(5, \"t\"); });\n    uiWire(g_uiW.s5b, function () { uiRowAct(5, \"v\"); });\n    uiWire(g_uiW.s5c, function () { uiRowAct(5, \"d\"); });\n    uiWire(g_uiW.s6a, function () { uiRowAct(6, \"t\"); });\n    uiWire(g_uiW.s6b, function () { uiRowAct(6, \"v\"); });\n    uiWire(g_uiW.s6c, function () { uiRowAct(6, \"d\"); });\n    uiWire(g_uiW.s7a, function () { uiRowAct(7, \"t\"); });\n    uiWire(g_uiW.s7b, function () { uiRowAct(7, \"v\"); });\n    uiWire(g_uiW.s7c, function () { uiRowAct(7, \"d\"); });\n    uiWire(g_uiW.addRect, function () { toast(\"拖框画矩形，抬手保存\"); capStart(\"rect\"); });\n    uiWire(g_uiW.addCircle, function () { toast(\"起点为圆心，拖动定半径\"); capStart(\"circle\"); });\n    uiWire(g_uiW.preview, function () { ovPreview(!g_ovW); uiRefresh(); });\n    uiWire(g_uiW.btnQuit, function () { uiClose(); });\n    log(\"ui wired\");\n    uiRefresh();\n    log(\"ui refreshed\");\n}\nfunction capClose() { try { if (g_capW) g_capW.close(); } catch (e) {} g_capW = null; g_cap = null; }\nfunction capStart(mode) {\n    capClose();\n    g_cap = { mode: mode, sx: 0, sy: 0, cx: 0, cy: 0 };\n    g_capW = floaty.rawWindow('<frame id=\"cap\"><canvas id=\"board\" layout_weight=\"1\"/></frame>');\n    g_capW.setSize(device.width, device.height);\n    g_capW.setTouchable(true);\n    g_capW.board.on(\"draw\", function (canvas) {\n        /* 线程池 draw 与 UI 线程 capClose/capStart 并发：快照局部，guard 后 cap 不会被并发置 null（TypeError 根因） */\n        var cap = g_cap;\n        if (!cap) return;\n        try { canvas.drawColor(colors.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR); } catch (e) {}\n        var dx = cap.cx - cap.sx, dy = cap.cy - cap.sy;\n        if (dx * dx + dy * dy < 400) return;\n        var p = new Paint(); p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(4); p.setColor(colors.GREEN);\n        if (cap.mode === \"circle\") canvas.drawCircle(cap.sx, cap.sy, Math.sqrt(dx * dx + dy * dy), p);\n        else canvas.drawRect(Math.min(cap.sx, cap.cx), Math.min(cap.sy, cap.cy), Math.max(cap.sx, cap.cx), Math.max(cap.sy, cap.cy), p);\n    });\n    g_capW.cap.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, { onTouch: function (v, ev) {\n        try {\n            var cap = g_cap, w = g_capW;\n            if (!cap || !w) return true;\n            var a = ev.getAction(), x = ev.getX(), y = ev.getY();\n            if (a === 0) { cap.sx = x; cap.sy = y; cap.cx = x; cap.cy = y; }\n            else if (a === 2) { cap.cx = x; cap.cy = y; try { w.board.postInvalidate(); } catch (e) {} }\n            else if (a === 1) {\n                var dx = x - cap.sx, dy = y - cap.sy;\n                if (dx * dx + dy * dy > 2500) {\n                    /* 存屏坐标：触摸是 view 相对坐标，加回窗体偏移（状态栏 inset） */\n                    var ox = 0, oy = 0;\n                    try {\n                        if (!g_ovLoc) g_ovLoc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);\n                        w.cap.getLocationOnScreen(g_ovLoc);\n                        ox = g_ovLoc[0]; oy = g_ovLoc[1];\n                    } catch (e) {}\n                    var n = vt.loadRegions().length + 1, r;\n                    if (cap.mode === \"circle\") r = { id: \"c\" + Date.now() % 100000, name: \"圆形\" + n, type: \"circle\", cx: Math.round(cap.sx + ox), cy: Math.round(cap.sy + oy), r: Math.round(Math.sqrt(dx * dx + dy * dy)), enabled: true };\n                    else r = { id: \"r\" + Date.now() % 100000, name: \"矩形\" + n, x1: Math.round(Math.min(cap.sx, x) + ox), y1: Math.round(Math.min(cap.sy, y) + oy), x2: Math.round(Math.max(cap.sx, x) + ox), y2: Math.round(Math.max(cap.sy, y) + oy), enabled: true };\n                    var all = vt.loadRegions(); all.push(r); vt.rgSave(all); ovSet(all); uiRefresh();\n                    toast(\"已保存 \" + r.name);\n                }\n                capClose();\n            }\n        } catch (e) {}\n        return true;\n    } }));\n}\nfunction uiClose() { capClose(); try { if (g_uiW) g_uiW.close(); } catch (e) {} g_uiW = null; }\n/* ---- watcher 启动器：仪式全包（接管/启动/订阅/双线程/退出清理/保活），业务只传 handlers ---- */\nfunction bootWatch(h) {\n    var MY = \"\" + Date.now() + \"_\" + Math.random();\n    try {\n        events.broadcast.on(\"vt-takeover\", function (tok) {\n            if (tok !== MY) { try { ovClose(); } catch (e) {} try { uiClose(); } catch (e2) {} try { vt.stop(); } catch (e3) {} exit(); }\n        });\n    } catch (e) {}\n    try { events.broadcast.emit(\"vt-takeover\", MY); } catch (e) {}\n    sleep(1500);\n    vt.ensure();\n    var c = vt.connect();\n    var regions = vt.loadRegions();\n    ovShow(regions);\n    /* B 方案：匹配在 vtouchd native 层（region add/clear + region_ev 推送），\n     * 这里只做：配置下发 + 事件分发到 handlers + overlay 手指绘制（pev 驱动）。 */\n    var FINGERS = {};\n    function pushRegions(rs) {\n        try {\n            c.send(\"region clear\");\n            for (var i = 0; i < rs.length; i++) {\n                var r = rs[i], en = (r.enabled === false ? 0 : 1);\n                if (r.type === \"circle\")\n                    c.send(\"region add \" + r.id + \" 1 \" + Math.round(r.cx) + \" \" + Math.round(r.cy) + \" \" + Math.round(r.r) + \" 0 \" + en);\n                else\n                    c.send(\"region add \" + r.id + \" 0 \" + Math.round(r.x1) + \" \" + Math.round(r.y1) + \" \" + Math.round(r.x2) + \" \" + Math.round(r.y2) + \" \" + en);\n            }\n        } catch (e) {}\n    }\n    function dispatch(line) {\n        var p, i, region, f, k;\n        if (line.indexOf(\"region_ev \") === 0) {          /* region_ev <id> <ev> <slot> <x> <y> */\n            p = line.split(\" \");\n            if (p.length === 6) {\n                region = null;\n                for (i = 0; i < regions.length; i++) if (regions[i].id === p[1]) { region = regions[i]; break; }\n                f = { slot: +p[3], x: +p[4], y: +p[5] };\n                if (region) {\n                    if (p[2] === \"down\" && h.onDown) h.onDown(region, f);\n                    else if (p[2] === \"up\" && h.onUp) h.onUp(region, f);\n                    else if (p[2] === \"enter\" && h.onEnter) h.onEnter(region, f);\n                    else if (p[2] === \"move\" && h.onMove) h.onMove(region, f);\n                    else if (p[2] === \"exit\" && h.onExit) h.onExit(region, f);\n                    if (p[2] === \"down\" || p[2] === \"enter\") ovFlash(region.id);\n                }\n            }\n            return;\n        }\n        if (line.indexOf(\"pev \") === 0) {                /* pev <slot> <down|move|up> <x> <y>: 只画手指 */\n            p = line.split(\" \");\n            if (p.length === 5) {\n                if (p[2] === \"up\") delete FINGERS[p[1]];\n                else FINGERS[p[1]] = { x: +p[3], y: +p[4], down: true };\n                var a = [], k2;\n                for (k2 in FINGERS) a.push(FINGERS[k2]);\n                ovUpdate(a);\n            }\n        }\n    }\n    vt.sub(c);\n    pushRegions(regions);\n    events.on(\"exit\", function () {\n        try { vt.stop(); } catch (e) {}\n        try { ovClose(); } catch (e2) {}\n        try { uiClose(); } catch (e3) {}\n    });\n    threads.start(function () {\n        log(\"vt-sub: on\");\n        var lastPing = 0;\n        for (;;) {\n            try {\n                for (;;) {\n                    var line = c.recv();\n                    if (line === null) {\n                        if (Date.now() - lastPing > 3000) { lastPing = Date.now(); c.send(\"ping\"); }\n                        sleep(10);\n                        continue;\n                    }\n                    dispatch(line);\n                }\n            } catch (err) {\n                log(\"vt-sub 重连: \" + err);\n                try { c.close(); } catch (e2) {}\n                sleep(1000);\n                /* 必须更新全局 c：pushRegions/dispatch 闭包引用它，否则配置下发到旧连接被吞 */\n                try { vt.ensure(); c = vt.connect(); vt.sub(c); pushRegions(regions); }\n                catch (e3) { sleep(2000); }\n            }\n        }\n    });\n    setInterval(function () {\n        try {\n            var rs = vt.loadRegions();\n            if (JSON.stringify(rs) !== JSON.stringify(regions)) {\n                regions = rs;\n                pushRegions(rs);\n                ovSet(rs);\n            }\n        } catch (e) {}\n    }, 2000);\n}\n\n";


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

/* ---- 纯库，无副作用：加载只定义函数，不执行任何动作 ---- */
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
