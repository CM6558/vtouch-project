/**
 * vtouch.js —— AutoJs6 客户端 SDK（**引用即用，全程一个文件**）
 *
 *   var vt = require("/sdcard/vtouch.js");
 *   var c = vt.connect();                            // 连上就能用；daemon 会自己起
 *   vt.finger().tap(540, 1200);                      // 自动挑空闲 slot
 *   vt.finger(3).down(100, 200).move(120, 240).up(); // 显式 slot
 *   vt.frame([{slot:0,state:"down",x:100,y:200},     // 多指合并进同一帧
 *             {slot:1,state:"down",x:300,y:200}]);
 *   // 脚本结束（AutoJs6 停止按钮 / 跑到结尾）→ 自动停 daemon、释放 EVIOCGRAB。
 *
 * 生命周期（都是幂等的，你不需要手动管）：
 *   - require 时：若 daemon 没在跑 → 用 root 起它（`/data/local/tmp/vtouchd_ui`，不带尺寸参数：
 *     核心自己问框架 `wm size` 取逻辑尺寸）；已 在跑则复用，不重复起。
 *   - connect() 时再兜一次（防止 daemon 中途被杀）。
 *   - 脚本退出（events.on("exit")）→ 自动停：**先 SIGTERM**（核心自己收尾：停面板 → 放 grab），
 *     最多等 2s，仍在才 SIGKILL。强杀（长按停止/系统回收）不会走 exit 回调，
 *     那种情况下 daemon 会留着 —— 用 vt.stop() 兜或重跑一次脚本即可。
 *
 * 不需要自动起停的场合：
 *   global.VTOUCH_NO_AUTOSTART = true;   // 写在 require 之前
 *   vt.keepRunning(true);                // 或退出时不想关（长驻 / 被别的脚本共用）
 *
 * 前置：设备已 root；`/data/local/tmp/vtouchd_ui` 存在（主机侧 `sh scripts/ui-deploy.sh deploy` 推一次即可，
 * 那是设备上的唯一文件；面板三件套由核心启动时自解包）。
 *
 * 自包含版（可选）：`python scripts/pack_client.py` 会把核心二进制 base64 内嵌进本文件
 * （产出 `build/vtouch_onefile.js`）。那种版本的 require 会在设备上没有该二进制、或版本不对时，
 * 自己把它写进去并校验 md5 —— 于是 AutoJs6 侧真正只有一个文件，设备上也不需要事先推任何东西。
 *
 * 坐标：daemon 用**竖屏逻辑坐标**（固定，不随旋转变）；本 SDK 不做旋转换算。
 */
"use strict";
var HOST = "127.0.0.1", PORT = 27183;
var BIN = "/data/local/tmp/vtouchd_ui", PROC = "vtouchd_ui";
var LOG = "/data/local/tmp/vt_ui_core.log";      /* daemon 日志（面板日志在 logcat tag VTouchUI） */
var SEND_LOCK = threads.lock();

var g_startedByUs = false;     /* daemon 是这次脚本起的吗（决定退出时要不要关） */
var g_keep = false;            /* vt.keepRunning(true) 后退出不关 */
var g_conn = null;             /* 当前连接（退出时先关） */

/* ---------- 自包含：内嵌核心二进制（源码态为 null；由 scripts/pack_client.py 填） ---------- */
var PAYLOAD = null;              /* <<PAYLOAD>> */
var PAYLOAD_MD5 = null;          /* <<PAYLOAD_MD5>> */
var PAYLOAD_SIZE = 0;            /* <<PAYLOAD_SIZE>> */
var PAYLOAD_TMP = "/sdcard/vtouchd_ui";   /* 先落共享存储，再 su 搬走（应用侧写不进 /data/local/tmp） */

function sh(cmd) { return shell(cmd, true); }
function trim(s) { return String(s == null ? "" : s).replace(/^\s+|\s+$/g, ""); }
function say(msg) { log("[vtouch] " + msg); }

function alive() {
    var r = sh("pidof " + PROC);
    return !!(r && trim(r.result));
}

/* 起 daemon：不在跑才起（root）。**不传 -w/-h** —— 核心自己问框架拿逻辑尺寸。 */
function start() {
    if (alive()) return true;
    var cmd = "D=" + BIN + ";[ -f $D ]||exit 11;"
        + "cd /data/local/tmp || exit 12;"
        + "nohup ./" + PROC + " -p " + PORT + " >" + LOG + " 2>&1 </dev/null & echo started";
    var r = sh(cmd);
    if (r && r.code === 11) {
        throw new Error("设备上缺 " + BIN + " —— 主机侧跑一次：sh scripts/ui-deploy.sh deploy");
    }
    for (var i = 0; i < 30 && !alive(); i++) sleep(100);
    if (!alive()) throw new Error("daemon 没起来，看 " + LOG);
    return true;
}

/* 停 daemon：SIGTERM 让核心自己收尾（先停面板、再放 EVIOCGRAB），超时才 SIGKILL。
 * 进程一退内核自动解 grab，物理触摸回系统直读。 */
function stop() {
    if (!alive()) return true;
    sh("kill -TERM $(pidof " + PROC + ") 2>/dev/null");
    for (var i = 0; i < 20 && alive(); i++) sleep(100);
    if (alive()) sh("kill -9 $(pidof " + PROC + ") 2>/dev/null");
    var ok = !alive();
    if (!ok) say("停不掉 " + PROC + "（可能要手动：su -c 'kill -9 $(pidof " + PROC + ")'）");
    return ok;
}

/* 设备上那个二进制的 md5（读不到返回空串） */
function deviceMd5(path) {
    var r = sh("md5sum " + path + " 2>/dev/null");
    var s = trim(r && r.result);
    return s ? s.split(/\s+/)[0] : "";
}

/* 自包含版专用：设备上没有该二进制、或版本不对时，用内嵌内容装上去并校验 md5。
 * 非内嵌（源码态）直接返回 —— 那时按老规矩用设备上已有的那份。 */
function installBinary() {
    if (!PAYLOAD) return;
    if (deviceMd5(BIN) === PAYLOAD_MD5) return;          /* 已经是这一版 → 只花一条命令 */
    say("设备上的核心不是这一版 → 从内嵌内容安装（" + PAYLOAD_SIZE + " 字节）");
    if (alive()) stop();                                  /* 正在跑就先停，否则覆盖会 Text file busy */
    var bytes = android.util.Base64.decode(PAYLOAD, android.util.Base64.DEFAULT);
    var fos = new java.io.FileOutputStream(PAYLOAD_TMP);
    try { fos.write(bytes); fos.flush(); } finally { try { fos.close(); } catch (e) {} }
    var r = sh("cp " + PAYLOAD_TMP + " " + BIN + " && chmod 755 " + BIN + " && rm -f " + PAYLOAD_TMP
             + " && md5sum " + BIN);
    var got = trim(r && r.result).split(/\s+/)[0];
    if (got !== PAYLOAD_MD5) throw new Error("安装校验失败：期望 " + PAYLOAD_MD5 + "，设备上得到 " + got);
    say("安装完成，md5 校验通过（" + got + "）");
}

/* 确保可用（require 与 connect 都会调，幂等） */
function ensure() {
    installBinary();
    if (alive()) return;
    start();
    g_startedByUs = true;      /* 只有"我们起的"才在退出时关掉 */
    say("daemon 已启动（pid " + trim(sh("pidof " + PROC).result) + "）");
}

function keepRunning(b) { g_keep = (b !== false); return g_keep; }
function startedByUs() { return g_startedByUs; }

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
/* 连上（必要时先把 daemon 拉起来；连不上会在 timeout 内重试） */
function connect(timeout) {
    timeout = timeout || 10000;
    if (!alive()) ensure();
    var t0 = Date.now(), last = null;
    for (;;) {
        try { g_conn = attach(connectOnce(timeout)); return g_conn; }
        catch (e) {
            last = e;
            if (Date.now() - t0 > timeout) throw new Error("连不上 daemon(127.0.0.1:" + PORT + "): " + last);
            sleep(300);
        }
    }
}
/* 给裸 socket 挂上 send/recv/drain/close/cmd（客户端帧必须掩码） */
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

/* finger([conn], [slot])：三种写法都吃 —— finger() / finger(3) / finger(conn, 3)。
 * （早期版本只接受 finger(conn, slot)，写成 finger(3) 不报错却会发出 "down NaN …"，这里兼容掉。） */
function finger(a, b) {
    var conn, slot;
    if (a && typeof a === "object" && a.send) { conn = a; slot = b; }
    else { conn = g_conn; slot = (a === undefined ? b : a); }
    if (!conn) conn = connect();
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
function frame(pts, conn) {
    if (!conn) conn = g_conn || connect();
    if (!conn.fingers) conn.fingers = {};
    var i, p;
    conn.send("begin_frame");
    try {
        for (i = 0; i < pts.length; i++) {
            p = pts[i];
            conn.send("point " + p.slot + " " + p.state + " " + Math.round(p.x) + " " + Math.round(p.y));
            if (conn.fingers[p.slot]) conn.fingers[p.slot].downState = p.state !== "up";
        }
    } finally {
        conn.send("end_frame");
    }
}
/* 逻辑尺寸与 raw 量程（返回 "res 1440 3168 raw 0 23040 0 50688" 这样的字符串） */
function res() {
    var c = g_conn || connect();
    return c.cmd("res");
}

/* ---------- 退出收尾：脚本结束自动关（只关"这次脚本起的"那个） ---------- */
events.on("exit", function () {
    try { if (g_conn) g_conn.close(); } catch (e) {}
    if (g_keep) { say("退出：keepRunning 生效，daemon 留着"); return; }
    if (g_startedByUs) {
        var ok = stop();
        say(ok ? "退出：daemon 已停（EVIOCGRAB 已释放）" : "退出：daemon 没停掉");
    }
});

/* require 即就绪：不在跑就起（可用 global.VTOUCH_NO_AUTOSTART = true 跳过） */
if (typeof global === "object" && global && global.VTOUCH_NO_AUTOSTART) {
    say("VTOUCH_NO_AUTOSTART：不自动起，用 vt.start() 手动起");
} else {
    try { ensure(); } catch (e) { say("自动启动失败：" + e); throw e; }
}

module.exports = {
    start: start, stop: stop, alive: alive, ensure: ensure,
    keepRunning: keepRunning, startedByUs: startedByUs,
    connect: connect, finger: finger, frame: frame, res: res,
    BIN: BIN, HOST: HOST, PORT: PORT
};
