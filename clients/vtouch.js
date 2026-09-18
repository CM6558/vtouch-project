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
 * 两条推送（都是单向；回调跑在子线程，h 里都带 t = **事件发生的墙钟毫秒**，与 Date.now() 同基准）：
 *   vt.onRegion([id,] [事件,] cb)   区域事件 down/enter/move/exit/up —— **按区域过滤**：手指滑出
 *                                   区域后就只剩一个 exit 了，所以要「追手指」得用下面这条。
 *   vt.onTouch([slot,] cb)          **物理触摸流**：任何物理手指的 down/move/up，不按区域过滤；
 *                                   vt.follow(slot, cb) 是它的简写（只跟一根手指）。
 *   两条流只报**物理**手指 —— 脚本自己注入的虚拟触点不会回流（防自激）。
 *
 * 典型用法「某手指在区域内按下 → 一路跟到抬起」：
 *   vt.onRegion("c1", "down", function (h) {
 *       var t = vt.follow(h.slot, function (e) {              // 只跟按下那根
 *           log(e.ev + " @" + e.x + "," + e.y);               // 移到哪都收得到（区域外也算）
 *           if (e.ev === "up") { log("历时 " + (e.t - h.t) + " ms"); t.stop(); }
 *       });
 *   });
 *
 * 坐标：daemon 用**竖屏逻辑坐标**（固定，不随旋转变）；本 SDK 不做旋转换算。
 */
"use strict";
var HOST = "127.0.0.1", PORT = 27183;
/* 客户端可用槽 `0..vslots-1`（服务端校验，默认 10；物理段在其之上） */
var g_physSlots = 64;            /* 物理槽上限（onTouch/follow 校验用，从 res 命令更新） */
var BIN = "/data/local/tmp/vtouchd_ui", PROC = "vtouchd_ui";
var LOG = "/data/local/tmp/vt_ui_core.log";      /* daemon 日志（面板日志在 logcat tag VTouchUI） */
/* 数据流读超时（毫秒，SO_MS）：**握手之后**每一轮读最多等这么久。它只描述「**整帧起点**没数据」
 * 这一件事（recv 读第一字节超时 → 返回 null = 本轮无数据），**不是掉线的判据** —— 掉线是对端
 * close 让 read() 返回 -1。帧**里面**迟到（一帧被 TCP 切开、后半段晚到）不在这道门限的语义里：
 * recv 读第一字节之后的部分走 FRAME_MS 的容错循环，单次超时只是继续等，不当掉线（I1）。
 * 别拿它当握手超时：握手用的是**调用方给的 timeout**（connect() 默认 10s；只有直接调
 * connectOnce() 且不传 timeout 时才落回 5s 兜底），握完立刻切到这个值。 */
var SO_MS = 200;
/* 一帧的「补齐总预算」（毫秒，FRAME_MS）：从读到帧第一字节算起，帧头剩余 + 帧体的补齐最多花这么久。
 * 期间每次读超时（SO_MS）都只是「这一小段还没到」→ 接着读；累计超过总预算才抛
 * 「半帧超时（连接可能已错位）」（那才是连接真不对了，按断开收尾）。见 recv / readFullTolerant。 */
var FRAME_MS = 5000;
var SEND_LOCK = threads.lock();
var g_handlersLock = threads.lock();

var g_startedByUs = false;     /* daemon 是这次脚本起的吗（决定退出时要不要关） */
var g_keep = false;            /* vt.keepRunning(true) 后退出不关 */
var g_conn = null;             /* 当前连接（退出时先关） */
var g_closing = false;         /* 我们自己正在收尾（exit 钩子 / vt.stop()）→ 读线程退出算正常收尾，只记日志不弹提示（F3） */

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
    g_closing = true;          /* 是我们自己要停核心：随后的事件通道关闭是**预期**的，不当掉线报（F3） */
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
/* 严格定长读：任何一次读超时都直接抛（非容错版）。**帧体的读取一律走下面的 readFullTolerant**
 * （帧内迟到不许当掉线，I1）—— 这个函数保留给「不允许迟到」的场合（握手侧的定长读要用它，别改成容错版）。 */
function readFull(ins, n) {
    var buf = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n), off = 0;
    while (off < n) {
        var k = ins.read(buf, off, n - off);
        if (k < 0) throw new Error("连接断开");
        off += k;
    }
    return buf;
}
/* 是不是「读超时」（SO_TIMEOUT 到点）：recv 只认这一种异常当「本轮无数据」，别的原样往外抛。
 * Java 的异常对象在 Rhino 里拿不到 instanceof（不同 classloader），所以按名字/文案认。 */
function isReadTimeout(e) {
    var nm = "";
    try { nm = String((e && e.name) || "") + " " + String((e && e.message) || "") + " " + String(e); } catch (e2) {}
    return nm.indexOf("SocketTimeout") >= 0;
}
/* 帧**内**容错读（I1）：第一字节之后的部分一律走这两个 —— 单次读超时只说明「这一小段还没到」
 * （一帧被 TCP 切开、后半段迟到 >SO_MS 很常见），继续读；累计超过 deadline（帧第一字节时算的
 * now + FRAME_MS）才抛「半帧超时（连接可能已错位）」当断开收尾。
 * 为什么要这样：以前这里直接用 readFull，抛出的 SocketTimeout 会被读线程当成掉线 → toast +
 * exit()，一次网络抖动就升级成「整个脚本被杀」。SO_MS 只描述整帧起点没数据，帧内迟到不是掉线。
 * 节奏由 SO_TIMEOUT 自己给（每次超时要等满 SO_MS 才回来），所以这个循环不会忙等。 */
function readByteTolerant(ins, deadline) {
    for (;;) {
        try { return ins.read(); }
        catch (te) {
            if (!isReadTimeout(te)) throw te;
            if (Date.now() >= deadline) throw new Error("半帧超时（连接可能已错位）");
        }
    }
}
function readFullTolerant(ins, n, deadline) {
    var buf = java.lang.reflect.Array.newInstance(java.lang.Byte.TYPE, n), off = 0, k;
    while (off < n) {
        try { k = ins.read(buf, off, n - off); }
        catch (te) {
            if (!isReadTimeout(te)) throw te;
            if (Date.now() >= deadline) throw new Error("半帧超时（连接可能已错位）");
            continue;
        }
        if (k < 0) throw new Error("连接断开");
        if (k === 0) {           /* 真实 socket 上不会返回 0（off<len）；真遇上就等预算到点 */
            if (Date.now() >= deadline) throw new Error("半帧超时（连接可能已错位）");
            continue;
        }
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
    /* 握手完了：socket 超时从「握手那个值（调用方给的 timeout，connect() 默认 10s；connectOnce
     * 直接调且不传才是 5000 兜底）」换成 SO_MS —— 之后每轮读最多等 200ms，**整帧起点**读超时代表
     * 本轮无数据（recv 返回 null），不是掉线；帧内迟到由 FRAME_MS 的容错循环兜住。见 recv 的注释。 */
    sock.setSoTimeout(SO_MS);
    return { sock: sock, out: out, ins: ins, mask: new java.util.Random() };
}
/* 连上（必要时先把 daemon 拉起来；连不上会在 timeout 内重试） */
function connect(timeout) {
    timeout = timeout || 10000;
    if (!alive()) ensure();
    var t0 = Date.now(), last = null;
    for (;;) {
        try {
            g_conn = attach(connectOnce(timeout));
            g_closing = false;               /* 新连接已建立 → 上一次「主动收尾」翻篇（F3 标志只描述当前连接） */
            if (!g_reader) startReader();    /* 断连后重连：reader 没在跑就重启 */
            /* 重连后核心的 sub_mask 已被 drop_client 清零，重新订阅 */
            var oldSubs = Object.keys(g_subbed);
            g_subbed = {};
            for (var i = 0; i < oldSubs.length; i++) {
                g_conn.send("sub " + oldSubs[i]);
                g_subbed[oldSubs[i]] = 1;
            }
            return g_conn;
        }
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
    /* 取一条文本消息。对外语义只有三种：有帧 → 返回字符串；**本轮无数据** → null；
     * 连接断开 → 抛异常。上游（startReader / drain / listRegions）就是按这三种写的，逻辑不用改。
     *
     * 实现是**阻塞读 + 读超时当「本轮无数据」**（F4），不再拿 `ins.available()` 判有没有数据：
     * available() **看不见对端 close** —— 对端关连接时它返回 0 而不抛异常（真机实测），于是
     * 「掉线」永远读不出来：读线程察觉不到、A2 的提示与收尾一次都不会触发（脚本一直当僵尸）。
     * 只有 read() 才有 EOF 判据：
     *   read() 抛 SocketTimeout → **整帧起点**本轮无数据 → return null（门限 SO_MS=200，见 connectOnce）；
     *   read() 返回 -1 → 对端关连接（EOF）→ 抛「连接断开」。
     * 所以这一层**不要再退回 available() 轮询**。
     * read() 抛其它异常（例如自己这边已经 close 掉的 socket）原样往外抛 —— 同样按断开收尾。
     *
     * I1：上面那条「读超时 = 本轮无数据」**只覆盖第一字节**（整帧起点）。第一字节之后的部分
     * （帧头剩余 + 帧体）走 readByteTolerant / readFullTolerant：帧内单次读超时（一帧被 TCP 切开、
     * 后半段迟到）只是继续等，累计超过 FRAME_MS 才抛「半帧超时（连接可能已错位）」。 */
    conn.recv = function () {
        var b0;
        try { b0 = conn.ins.read(); }
        catch (te) {
            if (isReadTimeout(te)) return null;                   /* 整帧起点本轮无数据，不是掉线 */
            throw te;
        }
        if (b0 < 0) throw new Error("连接断开");                  /* 读到 -1 = 对端关了（EOF） */
        /* 第一字节已到手 → 这一帧的补齐总预算开算；后面每一段迟到都从这里扣（I1） */
        var dl = Date.now() + FRAME_MS;
        var b1 = readByteTolerant(conn.ins, dl);
        if (b1 < 0) throw new Error("连接断开");
        var len = b1 & 127, i;
        if (len === 126) { var e = readFullTolerant(conn.ins, 2, dl); len = ((e[0] & 255) << 8) | (e[1] & 255); }
        else if (len === 127) throw new Error("帧过大");
        if ((b0 & 15) === 8) { try { conn.sock.close(); } catch (e2) {} throw new Error("服务端关闭"); }
        var p = readFullTolerant(conn.ins, len, dl), cs = [];
        for (i = 0; i < len; i++) cs.push(String.fromCharCode(p[i] & 255));
        return decodeURIComponent(escape(cs.join("")));
    };
    conn.drain = function () {
        if (g_reader) return;             /* 读线程在跑时别自己读：抢帧会丢事件、还会把帧读散 */
        try { while (conn.recv() !== null) {} } catch (e) {}
    };
    conn.close = function () { try { conn.sock.close(); } catch (e) {} };
    /* 发一条命令并等一行回包（调试用小工具）。
     * **回包必须由读线程转交**：连上之后这条 socket 只有一个读者（startReader），
     * 再自己 recv 就是两个读者抢帧 —— 抢输的那次拿不到回包，500ms 后返回 null。
     * 所以这里只往 g_reply 投一个盒子，读线程把"没人认领"的行投进来。
     * 同一时刻只支持一个等待者（并发调用会互相覆盖，先来那个照旧超时返回 null）。 */
    conn.cmd = function (line) {
        var box = { line: null };
        g_reply = box;                    /* 先挂盒子再发：回包可能比 send 返回还快 */
        try {
            conn.send(line);
            var dl = Date.now() + 500;
            for (;;) {
                if (box.line !== null) return box.line;
                if (Date.now() > dl) return null;
                sleep(5);
            }
        } finally { if (g_reply === box) g_reply = null; }
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
    var s = c.cmd("res");
    if (!s) { warn("res 没拿到回包（500ms 超时）—— 连接可能已断"); return s; }
    var m = s.match(/phys\s+(\d+)/);
    if (m) g_physSlots = parseInt(m[1], 10);
    return s;
}

/* ---------- 退出收尾：脚本结束自动关（只关"这次脚本起的"那个） ---------- */
events.on("exit", function () {
    g_closing = true;          /* 先置位再关：随后读线程因连接关闭而退出 = 正常收尾，不是掉线（F3） */
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

/* ---------- 区域事件订阅（高层 API，与原 bundle 的 vt.onRegion 同语义） ----------
 *   vt.onRegion(cb)                    所有区域 + down/up/enter/exit（默认不含 move）
 *   vt.onRegion(cb, "up")              所有区域 + 只要抬起
 *   vt.onRegion("c1", "down,move", cb) 指定区域 + 指定事件
 *   vt.onTouch(cb)                     物理触摸流：任何手指的 down/move/up（不按区域过滤）
 *   vt.onTouch(3, cb) / vt.follow(3,cb) 只跟 slot 3 这根手指 —— 它移到哪、何时抬起都拿得到
 *                                      （区域事件只覆盖"区域内"，追手指要用这条流）
 * events 省略 = down/up/enter/exit；"*" / "any" = 全部（含 move）；也可给数组。
 * 回调收到 h = { id, ev, slot, x, y, t }，跑在**分发线程**里，**串行**执行（事件到达顺序 == 回调
 * 顺序，不并发）：回调里长 sleep 会**推迟后面的每一条事件** —— 要慢动作/长动作请自己在回调里
 * threads.start(...)。move 是「状态」不是消息：同一 (区域, 槽) 只保留**最新一条**（后到的原地
 * 覆盖先前那条，简报一条不丢、队列不会积压）；要完整轨迹就自己按 h.t 记点，别指望每条采样都送到。
 * 掉线（面板退出 / 被新实例顶掉）会自动提示 + 摘掉全部 handler，脚本可正常结束。
 *   t 是**事件发生的墙钟毫秒**（与 Date.now() 同基准）：算按压时长用 up.t - down.t、
 *   做防抖/节流、量「手指按下到脚本收到」的延迟都能用（h.t 是手指那一刻，不是回调那一刻）。
 * 返回 { stop() }：停监听（不关面板；面板归脚本退出时的 exit 钩子收）。
 * 掉线收尾（清保活定时器 + 摘 handler + 停分发线程）在 vt.keepRunning(true) 时不做，只提示 ——
 * 那种场合收尾归脚本自己管。
 * 区域 id 写错 / 被禁用会在启动时提示，不会让你干等到怀疑人生。 */
function parseEvents(evs) {
    if (evs === undefined || evs === null) return { down: 1, up: 1, enter: 1, exit: 1 };
    var arr = (typeof evs === "string") ? evs.split(",") : evs, m = {}, i;
    if (!arr || !arr.length) return { down: 1, up: 1, enter: 1, exit: 1 };
    for (i = 0; i < arr.length; i++) {
        var e = trim(String(arr[i]));
        if (e === "" || e === "*" || e === "any") return { down: 1, up: 1, enter: 1, exit: 1, move: 1 };
        m[e] = 1;
    }
    return m;
}
function warn(msg) {
    if (typeof toastLog === "function") toastLog(msg); else log("[vtouch] " + msg);
}
/* ---------- 单一读取线程 ----------
 * 区域事件(region_ev) 与物理触摸流(phys_ev) 走的是**同一条 socket**，各起一个读线程会互相抢消息
 * （谁先 read 到就是谁的）。所以这里只有一个读取者，按前缀分派给各个注册的回调。 */
var g_reader = null;              /* 读线程（只起一个） */
var g_reply = null;               /* { line } 正在等命令回包的人；读线程把没人认领的行投进来（cmd 用） */
var g_listSawEnd = 0;             /* 这次 region list 有没有见到末行 end N（0 = 超时/被截断） */
var g_listEndN = -1;              /* 末行 `end N` 里声明的行数（-1 = 没见到末行 / N 没解析出来） */
var g_subbed = {};                /* 已订阅的通道（幂等，避免重复发 sub） */
var g_regionHandlers = [];        /* {id, want, cb} */
var g_touchHandlers = [];         /* {slot, cb} */
var g_lastPhysDown = {};           /* { slot: { ev, slot, x, y, t } } — 缓存最近一次 phys_ev down */
var g_listCollector = null;       /* 正在等 region list 回包时，把 region/end 行收进这个数组 */

/* ---------- 事件派发：一条常驻分发线程 + 「位置只留最新」的合并表 ----------
 * 读线程只做「解析 + 入队」，回调由**分发线程串行**执行（事件到达顺序 == 回调顺序，不重排、不并发）。
 *
 * 入队分两类处理（本文件唯一的取舍点）：
 *   · **简报** down/up/enter/exit：少、且一条都不能丢（丢一条脚本状态就错）⇒ 逐条入队，永不丢弃；
 *   · **位置** move：极多（真机实测平均 198 条/s、峰值 634 条/s）⇒ 同一 (回调, 区域/槽) 只保留
 *     **最新一条**，新到的**原地覆盖**旧的那条 —— 位置不变 ⇒ 与简报的相对顺序不变。
 * 为什么这么做：move 是**状态**不是消息（脚本要的是「手指现在在哪」，不是「8ms 前在哪」）。
 * 以前按 FIFO 逐条排队、队列只有 256 格 ⇒ 回调一旦阻塞（长 sleep / 同步注入）0.5~1.3 s 就栽满，
 * 于是开始丢事件 + 每秒一次「队列已满」；而那条提示是**攥着 g_queueLock** 弹 Toast 的
 * （AutoJs6 的 Toast 要回主线程弹并同步等，最多 1 s）⇒ 读线程一起停，越堵越堵。
 * 改成合并后，队列长度只由「订阅者 × (区域/槽) 组合数」决定，与事件速率无关 ⇒ 结构上不会满。
 * 覆盖的判据见 mergeMove()：跨过简报（enter/exit/down/up）不许合并 —— 否则「离开又进入」之后的
 * 位置会顶到 exit 前面，顺序就错了。
 * 实现用 JS 数组 + threads.lock() 守护（不用 java.util.concurrent：Rhino 往 Java 集合里
 * 塞 JS 对象有边界问题）。分发线程惰性起、幂等：offer() 发现它不在就起一条，
 * 掉线收尾置停止标志让它退出，之后再次 offer() 会重新起一条。 */
var Q_SOFT = 512;                 /* 积压告警阈值（只记日志、不丢；结构上到不了） */
var Q_HARD = 4096;                /* 兜底上限（破了丢最旧一条并计数；结构上不该发生） */
var g_queue = [];                 /* 待分发 { cb, h }：数组顺序 == 回调顺序 */
var g_queueLock = threads.lock();
/* 空闲唤醒用（**AutoJs6 真机已验证可用**：`lock.newCondition()` / `await(ms, TimeUnit.MILLISECONDS)` /
 * `signal()` / `signalAll()` 全通，且 await 期间锁是释放的 ⇒ 入队方能照常 push+signal，不会丢唤醒）。
 * 判据脚本见 build/_dev/vt_cond_probe.js（探针输出：T1 超时 201ms、T2 signal 300ms 唤醒、T3 等待期取锁成功）。 */
var g_queueCond = g_queueLock.newCondition();
var g_queueDropped = 0;           /* 兜底丢弃条数（正常恒为 0：简报不丢、move 合并） */
var g_lastDropWarn = 0;           /* 上次积压/丢弃日志的墙钟毫秒（限频用） */
var g_dispatchRunning = false;    /* 分发线程在跑吗 */
var g_dispatchStop = false;       /* 让分发线程退出（掉线收尾置位） */

/* 分发线程主体：串行取队首 → 同步调用 cb(h)。队列空 → 在条件变量上等 signal（**不轮询**）；
 * 被要求停且队空 → 退（不留空转线程）。
 * 为什么用条件变量：原来空队列 `sleep(5)` = 空闲 **200 次/秒**唤醒 JS 引擎（主线程=应用 UI 线程，
 * 长期挤占）。`await()` 期间线程阻塞在 Java 层，不执行任何脚本。保留 1000ms 超时兜底：万一漏一次
 * signal 或 await 被中断，最多 1 秒后自己重查队列 ⇒ 空闲 1 次/秒（原 1/200），换来「永不死等」。 */
function dispatchLoop() {
    try {
        for (;;) {
            var job = null;
            g_queueLock.lock();
            try {
                if (g_queue.length) job = g_queue.shift();
                else if (g_dispatchStop) break;                                              /* 已入队的发完才退 */
                else g_queueCond.await(1000, java.util.concurrent.TimeUnit.MILLISECONDS);     /* 等 signal；超时只是兜底 */
            } finally { g_queueLock.unlock(); }
            if (job) {
                try { job.cb(job.h); } catch (e) { warn("回调出错：" + e); }
            }
        }
    } finally { g_dispatchRunning = false; }
}
/* 合并判据（**必须在 g_queueLock 里调**：只读数组、不做任何 I/O）：
 * 从队尾往前找同一 (回调, 区域, 槽) 的那一条 —— 找到的是 move 就原位覆盖并返回 1；
 * 找到的是简报（enter/exit/down/up）说明中间隔着事件，**不许跨过去合并**（返回 0，照常入队）。
 * 键：区域事件 = cb + h.id + h.slot；物理流事件 = cb + h.slot（phys_ev 没有 id 字段）。
 * 把 cb 算进键是为了**多订阅者隔离**：两个 handler 订同一个区域时，各自拿到自己的那条 move。 */
function mergeMove(h, cb) {
    var key = (h.id === undefined ? "" : h.id) + "|" + h.slot, q, i;
    for (i = g_queue.length - 1; i >= 0; i--) {
        q = g_queue[i];
        if (q.cb !== cb || !q.h) continue;
        if (((q.h.id === undefined ? "" : q.h.id) + "|" + q.h.slot) !== key) continue;
        if (q.h.ev !== "move") return 0;      /* 中间隔了简报 → 不许跨过去合并（顺序会错） */
        q.h = h;                              /* 原地替换：位置不变（顺序不变），数据是最新的 */
        return 1;
    }
    return 0;
}
/* 入队（取代原来的 fire()）：只入队，**绝不** threads.start —— 每条事件起一条线程会线程风暴且不保序。
 * 简报逐条入队；move 先试合并（见 mergeMove）⇒ 队列不再有「满了丢」这条路径。
 * 所有提示/日志一律**在锁外**、且用 log（不是 toastLog —— AutoJs6 的 Toast 要回主线程弹并同步
 * 等待，攥着 g_queueLock 弹就会把读线程一起停住，那是「越堵越堵」的正反馈）。 */
function offer(h, cb) {
    var start = false, note = null, now;
    g_queueLock.lock();
    try {
        if (h && h.ev === "move" && mergeMove(h, cb)) return true;   /* 合并成功：不占新格子 */
        if (g_queue.length >= Q_HARD) {                              /* 兜底：结构上不该发生 */
            g_queue.shift();
            g_queueDropped++;
            now = Date.now();
            if (now - g_lastDropWarn >= 1000) {
                g_lastDropWarn = now;
                note = "事件积压超过兜底上限（" + Q_HARD + "）—— 丢弃最旧一条，累计 " + g_queueDropped + " 条";
            }
        } else if (g_queue.length >= Q_SOFT) {                       /* 只诊断：回调太慢，延迟在涨 */
            now = Date.now();
            if (now - g_lastDropWarn >= 1000) {
                g_lastDropWarn = now;
                note = "事件积压 " + g_queue.length + " 条（回调太慢：回调里别 sleep/注入，长活请 threads.start）";
            }
        }
        g_queue.push({ cb: cb, h: h });
        /* 唤醒可能在 await 的派发线程：push 与 signal 在同一把锁里 ⇒ 不会丢唤醒。
         * 合并路径提前 return、不 signal 也是安全的：能合并说明队里已有格子 ⇒ 派发线程要么正在处理、
         * 要么已被上一次 signal 唤醒，此时没有等待者。 */
        g_queueCond.signal();
        if (!g_dispatchRunning) { g_dispatchRunning = true; g_dispatchStop = false; start = true; }
    } finally { g_queueLock.unlock(); }
    if (note) say(note);
    if (start) threads.start(dispatchLoop);
    return true;
}
function dispatchRegion(s) {
    var p = s.split(" ");
    var h = { id: p[1], ev: p[2], slot: parseInt(p[3], 10), x: parseInt(p[4], 10), y: parseInt(p[5], 10),
              t: p.length > 6 ? parseInt(p[6], 10) : null };
    g_handlersLock.lock();
    var snapshot;
    try { snapshot = g_regionHandlers.slice(); } finally { g_handlersLock.unlock(); }
    for (var i = 0; i < snapshot.length; i++) {
        var H = snapshot[i];
        if (H.id && H.id !== h.id) continue;
        if (!H.want[h.ev]) continue;
        offer(h, H.cb);
    }
}
function dispatchTouch(s) {
    var p = s.split(" ");
    var h = { ev: p[1], slot: parseInt(p[2], 10), x: parseInt(p[3], 10), y: parseInt(p[4], 10),
              t: p.length > 5 ? parseInt(p[5], 10) : null };
    if (h.ev === "down") g_lastPhysDown[h.slot] = h;   /* 缓存 down，供新注册的 handler 补收 */
    if (h.ev === "up") delete g_lastPhysDown[h.slot];   /* 抬起后清缓存 */
    g_handlersLock.lock();
    var snapshot;
    try { snapshot = g_touchHandlers.slice(); } finally { g_handlersLock.unlock(); }
    for (var i = 0; i < snapshot.length; i++) {
        var H = snapshot[i];
        if (H.slot !== null && H.slot !== h.slot) continue;
        offer(h, H.cb);
    }
}
function startReader() {
    if (g_reader) return;
    g_reader = threads.start(function () {
        try {
            while (true) {
                var s = null;
                try { s = g_conn ? g_conn.recv() : null; } catch (e) { break; }
                if (s === null || s === undefined) { sleep(10); continue; }
                /* 帧先按**类**分派，事件/表回包再按**行**切：现核心的 `region list` 是**一区一帧**
                 * （src/vt_ws.c 逐行 outq_push_text），事件也是一帧一行；逐行处理留着是因为**旧核心
                 * 会把整表拼成一个帧**发回来（兼容老固件）。不按行切的话，旧核心那帧的末行 end
                 * 永远走不到「收尾」分支，收集器要等满 1000ms 超时才返回，还会把整块塞进数组第 1 个
                 * 元素；反过来按「整块」写新代码也是错的（一区一帧 ⇒ 一次只回第一行）。 */
                var lines = String(s).split(/\r?\n/), li, ln;
                if (s.indexOf("region_ev ") === 0) {
                    for (li = 0; li < lines.length; li++) if (lines[li]) dispatchRegion(lines[li]);
                    continue;
                }
                if (s.indexOf("phys_ev ") === 0) {
                    for (li = 0; li < lines.length; li++) if (lines[li]) dispatchTouch(lines[li]);
                    continue;
                }
                if (g_listCollector && (s.indexOf("region ") === 0 || s.indexOf("end ") === 0)) {
                    for (li = 0; li < lines.length; li++) {
                        ln = lines[li];
                        if (!ln) continue;
                        /* 末行 end N：N 是**表里声明的条数**，收完唤醒等它的人（I2 靠它对账行数） */
                        if (ln.indexOf("end ") === 0) { g_listSawEnd = 1; g_listEndN = endCount(ln); g_listCollector = null; break; }
                        if (g_listCollector) g_listCollector.push(ln);
                    }
                    continue;
                }
                /* 命令回包：**整帧原文**交给等它的人 —— 注意这条路径**一次只回一帧**：`region list`
                 * 一区一帧，所以 cmd("region list") 拿到的只是第一行、也拿不到 end N；整表用
                 * listRegions()（它按行收 + 拿 end N 对账）。 */
                if (g_reply) { g_reply.line = s; continue; }
            }
        } finally {
            g_reader = null;    /* 线程退出（断连）→ 允许下次 startReader 重启 */
            g_lastPhysDown = {}; /* 清缓存：重连后不会补发断连前的 down */
            /* 读线程为什么退出？先分「是不是我们自己主动收尾」这一档（F3）：
             *   g_closing（exit 钩子 / vt.stop() 置位）→ **正常收尾**：只打一条普通日志，
             *     绝不弹「事件通道已断开」——脚本一切正常却报错就是假警报（A2 的回归）；
             *   否则才是掉线（以前只清 g_reader，脚本照旧「活着」但事件永不来）：判因看
             *     **两条进程** —— `pidof vtouchd_ui`（核心）与 `pidof vtouch-ui`（面板），
             *     只有两个都还在才说明是「被新实例顶掉」，否则取「面板退出」（核心没了也
             *     算这一档：面板按退出是先让核心退，见下面 else 分支的注释，I6）→ toast 提示。
             * 标志一次性消费（下一条读线程由 connect() 建新连接时归零）。
             * 下面这段收尾**两条路完全一样**（逃生门 g_keep 除外）：保活定时器、handler 表、
             * 分发线程都要拆干净 —— 自己关的那次也不能留假活状态；只有判因那两条 root 命令
             * （pidof）在掉线段才花。唯一的分歧在**结束脚本**那一步（见下面的 F5：只有真掉线
             * 且非逃生门才调 exit()）。g_listCollector 保持现有语义（等它的人自己有 1000ms
             * 超时；读线程不等它，不会被卡住）。
             *
             * F5：这段收尾**每一步各自 try/catch**，任何一步炸掉都不许吃掉提示、也不许挡住
             * 后面的步骤 —— 收尾只跑一半 = 脚本半死（定时器还在 / handler 还挂着 / 分发线程
             * 还转着），AutoJs6 就永远不会结束。判因那条 root 命令单独包（`pidof vtouch-ui`
             * 探不到就按「面板还在」取文案）：绝不允许因为一条 shell 抛错而一句提示都没有。
             *
             * F5 为什么必须在末尾显式 `exit()`（A2 的「本脚本可正常结束」要真的成立）：
             *   AutoJs6 只要**建过 interval 或还有子线程**，收尾后引擎就不会自行结束。真机实测：
             *   · 30s 定时器被（子线程里）clearInterval 之后，脚本 40s 仍不结束（30s 期间唤醒也不结束）；
             *   · 2s 定时器清了 → 11ms 就结束（复现两次）；
             *   · 子线程全部结束 → 脚本结束。
             *   ⇒ 光把定时器 / handler / 分发线程拆干净**还不够**，收尾末尾必须显式 `exit()` 让
             *     脚本真的结束（否则「本脚本可正常结束」是句空话）。
             *   `exit()` **会照常触发 `events.on("exit")` 钩子**（真机实测：11ms 后触发，钩子里的
             *     写盘正常完成）⇒ 用户挂在退出钩子上的收尾不会因为这次 exit() 丢。
             *   只有这一档（真掉线且非逃生门）调它：
             *   · `closing`（g_closing 真）= **脚本自己在收尾**（exit 钩子里再 exit 是自己套自己；
             *     vt.stop() 之后脚本还有事要做）→ 只记日志，不替它决定结束；
             *   · 逃生门 `g_keep`（vt.keepRunning(true)）→ 收尾整个归脚本自己管，更不调 exit()。
             *   非 AutoJs6 环境（离线 harness / 别的 JS 宿主）根本没有 `exit` 这个全局：调用会抛
             *   ReferenceError，被这一层的 catch 吃掉，收尾的其余部分照常完成、也不许抛出去。 */
            var closing = g_closing;
            g_closing = false;
            if (closing) {
                try { say("脚本收尾：事件通道已关闭（不再收事件）"); } catch (e) {}
            } else {
                /* 判因（I6）：**核心与面板一起看**。只有「核心还在 + 面板还在」才可能是被新实例顶掉
                 * （新客户端连上会踢掉旧连接，核心与面板都活着）；否则一律取「面板退出」——
                 * 面板按「退出」是**先给核心 SIGTERM、核心先退**，面板要等自己下一拍（~5-40ms）
                 * 才消失，只看面板会把这一档误报成「被新实例顶掉」，让人去找并不存在的第二个实例。
                 * 两条探不到（shell 抛错 / pidof 不可用）都按「还在」处理，保持原本文案。
                 * 两步各自 try/catch：判因炸了也绝不许吞掉提示（F5）。 */
                var coreUp = true, uiUp = true;
                try { coreUp = !!(trim(sh("pidof " + PROC).result)); } catch (e) {}
                try { uiUp = !!(trim(sh("pidof vtouch-ui").result)); } catch (e) {}
                try {
                    warn((coreUp && uiUp) ? "事件通道已断开（被新实例顶掉）→ 停止监听，本脚本可正常结束"
                                          : "事件通道已断开（面板退出）→ 停止监听，本脚本可正常结束");
                } catch (e) {}
            }
            if (g_keep) {
                /* 逃生门 vt.keepRunning(true)：只提示，定时器/handler/分发线程/exit 全归脚本自己管 */
            } else {
                if (g_keepAlive) { try { clearInterval(g_keepAlive); } catch (e) {} g_keepAlive = null; }
                try {
                    g_handlersLock.lock();
                    try { g_regionHandlers = []; g_touchHandlers = []; } finally { g_handlersLock.unlock(); }
                } catch (e) {}
                /* 让分发线程「把已入队的发完就退」，不留空转线程。注意：**下面掉线档那句 exit() 不会
                 * 等它** —— exit() 当场结束整个脚本，队列里还没轮到的那些（简报 + 每 (区域/槽) 一条
                 * move）就永远发不出去了。要「尾部事件也发完」就不能在这条路上 exit()（这行本身只表达
                 * 「分发线程不空转」，不是「队列一定发得完」）。
                 * 置位**要持锁 + signal**：等待中的派发线程正在 `await()`，不唤醒它就得等满 1000ms 超时
                 * （signal 必须在持锁时调，否则抛 IllegalMonitorStateException）。 */
                try {
                    g_queueLock.lock();
                    try { g_dispatchStop = true; g_queueCond.signal(); } finally { g_queueLock.unlock(); }
                } catch (e) {}
                /* **最后**一步：上面的清理都做完了才结束脚本（放在前面会把收尾砍掉一半）。
                 * closing 为真 = 是我们自己要收尾，不替脚本决定结束（理由见上面的注释块）；
                 * 掉线档调它 = 丢弃队列里剩下的事件、当场结束脚本（见上一段的取舍）。 */
                if (!closing) { try { exit(); } catch (e) {} }
            }
        }
    });
}
/* 订阅一个通道（幂等） */
function subscribe(ch) {
    var conn = g_conn || connect();
    if (!g_subbed[ch]) { conn.send("sub " + ch); g_subbed[ch] = 1; }
    return conn;
}
/* 末行 `end N` 里的 N（表里声明的条数）；取不到数字返回 -1（那就只按「见过末行」判完整性）。 */
function endCount(line) {
    var m = String(line).match(/(\d+)/);
    return m ? parseInt(m[1], 10) : -1;
}
/* region list 回包对账（I2）：光看「见过 end N」不够。核心的 N+1 帧现在是**解锁后**逐条入出站队列
 * （64 格；满时丢**这一帧**、不挤已排队的旧数据 —— src/vt_queue.c 的 outq_push_text_keep），而 poll 线程
 * 一轮内没有排空机会 ⇒ 队满时常见是**后缀连同 end N 一起丢**，此时走「没见过末行」那条判据；
 * 若 end N 恰好发出去、前面的行被丢，就用它声明的 N 与实收行数对账。两条路都报，不静默给半张表。 */
function listReconcile(out) {
    if (!g_listSawEnd) { warn("region list 回包不完整（超时或被截断）—— 只拿到 " + out.length + " 条"); return; }
    if (g_listEndN >= 0 && g_listEndN !== out.length) {
        warn("region list 回包不完整：收到 " + out.length + " 行，表里声明 " + g_listEndN + " 条");
    }
}
/* 取区域表（region list 回 N 行 region ... + 一行 end N）。读线程已在跑时走收集器，不抢消息。 */
function listRegions(arg) {
    var conn = (arg && arg.recv) ? arg : (g_conn || connect());
    var out = [], dl, s, ls, i;
    g_listSawEnd = 0;
    g_listEndN = -1;
    if (g_reader) {
        g_listCollector = out;
        conn.send("region list");
        dl = Date.now() + 1000;
        while (g_listCollector !== null && Date.now() < dl) sleep(5);
        g_listCollector = null;
        listReconcile(out);
        return out;
    }
    conn.drain();
    conn.send("region list");
    dl = Date.now() + 1000;
    for (;;) {
        s = conn.recv();
        if (s) {
            ls = String(s).split(/\r?\n/);          /* 一帧可能是多行（旧核心整块发） */
            for (i = 0; i < ls.length; i++) {
                if (!ls[i]) continue;
                if (ls[i].indexOf("end ") === 0) { g_listSawEnd = 1; g_listEndN = endCount(ls[i]); break; }
                out.push(ls[i]);
            }
            if (g_listSawEnd) break;
        } else if (Date.now() > dl) break;
        else sleep(5);
    }
    listReconcile(out);
    return out;
}
/* 主线程保活：AutoJs6 里主线程一结束脚本就退，事件还没来就白等。最后一个订阅停掉时关掉它。
 * tick 间隔 30s（原来是 1000）：主线程 = AutoJs6 应用的 UI 线程，每次 tick 都要抢 JS 引擎锁；
 * 保活只需要「主线程别退出」，不需要秒级心跳 —— 历史上每秒空 tick 就是「每 1~2 秒卡一下」的成因。 */
var g_keepAlive = null;      /* 保活定时器（注意别和 keepRunning 的 g_keep 混了） */
function keepAlive() { if (!g_keepAlive) g_keepAlive = setInterval(function () {}, 30000); }
function dropHandler(arr, H) {
    g_handlersLock.lock();
    try {
        for (var i = 0; i < arr.length; i++) if (arr[i] === H) { arr.splice(i, 1); break; }
    } finally { g_handlersLock.unlock(); }
    if (!g_regionHandlers.length && !g_touchHandlers.length && g_keepAlive) {
        try { clearInterval(g_keepAlive); } catch (e) {}
        g_keepAlive = null;
    }
}
/* onRegion([id,] [事件,] cb)：区域事件（五事件，按区域过滤）。 */
function onRegion(a, b, c) {
    var id = null, evs = null, cb;
    if (typeof a === "function") cb = a;
    else if (typeof a === "string" && typeof b === "function") { id = a; cb = b; }
    else { id = a; evs = b; cb = c; }
    if (typeof cb !== "function") throw new Error("onRegion 需要一个回调函数");
    var H = { id: id, want: parseEvents(evs), cb: cb };
    subscribe("region");
    startReader();
    if (id) {                                   /* 查表：id 写错/被停用立刻提示，不让你干等到怀疑人生 */
        var rows = listRegions(), hit = false, i, tries = 0;
        /* 区域表是**面板**起来后从 regions.conf 载进核心的，而核心的监听早于面板
         * ⇒ require 后立刻 onRegion 可能问到空表。空表不能当成「id 写错了」，
         * 否则每次开机第一跑都会误报「面板里没有区域 x」（真机撞过）。 */
        while (!rows.length && tries < 4) { sleep(300); rows = listRegions(); tries++; }
        for (i = 0; i < rows.length; i++) {
            var p = rows[i].split(" ");
            if (p[1] === id) { hit = true; if (p[p.length - 1] === "0") warn("区域 " + id + " 目前是停用状态"); }
        }
        if (!hit) {
            if (!rows.length) warn("区域表还是空的（面板可能还在载入）——暂时没看到区域 " + id + "，稍后可再试");
            else warn("面板里没有区域 " + id + "（现有：" + rows.join(" | ") + "）");
        }
    }
    g_handlersLock.lock();
    try { g_regionHandlers.push(H); } finally { g_handlersLock.unlock(); }
    keepAlive();
    return { stop: function () { dropHandler(g_regionHandlers, H); } };
}
/* onTouch([slot,] cb)：**物理触摸流** —— 不按区域过滤，按下之后一路跟到抬起。
 * 典型用法：在 onRegion 里收到 down → 记住 h.slot → 之后用 onTouch(slot, cb) 跟这根手指，
 * 它移到哪、什么时候抬起都拿得到（哪怕早就滑出了那个区域）。
 * 回调收到 h = { ev, slot, x, y, t }，ev ∈ down/move/up（move 只在位置变化时报）。 */
function onTouch(a, b) {
    var slot = null, cb;
    if (typeof a === "function") { cb = a; }
    else { slot = (a === undefined || a === null) ? null : Math.round(a); cb = b; }
    if (typeof cb !== "function") throw new Error("onTouch 需要一个回调函数");
    if (slot !== null && (slot < 0 || slot >= g_physSlots)) throw new Error("slot 0~" + (g_physSlots - 1));
    var H = { slot: slot, cb: cb };
    subscribe("phys");
    startReader();
    /* 同 slot 已有 handler → 先停掉旧的，避免同一 up 被两个 handler 各收到一次 */
    g_handlersLock.lock();
    try {
        if (slot !== null) {
            for (var i = g_touchHandlers.length - 1; i >= 0; i--) {
                if (g_touchHandlers[i].slot === slot) {
                    g_touchHandlers.splice(i, 1);
                    break;
                }
            }
        }
        g_touchHandlers.push(H);
    } finally { g_handlersLock.unlock(); }
    keepAlive();
    /* 补发缓存的 down：如果该 slot 刚按下但 follow 注册晚了，补一条 down */
    if (slot !== null && g_lastPhysDown[slot]) {
        offer(g_lastPhysDown[slot], cb);
    }
    return { stop: function () { dropHandler(g_touchHandlers, H); } };
}
/* follow(slot)：只跟一根手指的便捷写法（等价 onTouch(slot, cb)） */
function follow(slot, cb) { return onTouch(slot, cb); }

module.exports = {
    start: start, stop: stop, alive: alive, ensure: ensure,
    keepRunning: keepRunning, startedByUs: startedByUs,
    connect: connect, finger: finger, frame: frame, res: res,
    onRegion: onRegion, listRegions: listRegions, onTouch: onTouch, follow: follow,
    BIN: BIN, HOST: HOST, PORT: PORT
};
