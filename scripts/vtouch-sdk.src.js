"use strict";
/* VTouch 精简 SDK 主源：只保留 Finger 生命周期 + 原子帧，时序由业务 sleep 控制。
 * 由 scripts/build_sdk.py 压缩分发到 clients/plugins/vtouch.js 与 APK 胶水层，勿直接改分发文件。
 * 用法: var vt = new VTouch(); vt.ready(); vt.finger().tap(540, 1200);
 *   耗时操作（tap/swipe/ready 内的 sleep）请放在 threads.start() 业务线程，主线程留给事件循环。
 */
var VTouch = function (options) {
    options = options || {};
    this.url = options.url || "ws://127.0.0.1:27183";
    this.width = options.width || device.width;
    this.height = options.height || device.height;
    this.timeout = options.timeout || options.connectTimeout || 10000;
    this.autoStop = options.autoStop !== false;
    this.ws = null;
    this.queue = [];
    this.opened = false;
    this.connecting = false;
    this.closed = false;
    this.fingers = {};
    this.serviceStarted = false;
    this.onReady = null;
    var self = this;
    if (options.autoConnect !== false) this.connect();
    events.on("exit", function () {
        try { self.close(); } finally {
            if (self.autoStop) { try { self.stopService(); } catch (e) { log("[vtouch] stop: " + e); } }
        }
    });
};
VTouch.prototype.startService = function () {
    if (this.serviceStarted) return this;
    /* vtouchd 单进程优先；/data/local/tmp 下无 vtouchd 时回滚旧双进程。健康则直接返回。 */
    var cmd = "B=/data/local/tmp/vtouch-runtime;D=/data/local/tmp/vtouchd;M=/data/local/tmp/vtouchmerge;W=/data/local/tmp/vtouchws;"
        + "[ -f $B/vtouchd.pid ]&&kill -0 $(cat $B/vtouchd.pid 2>/dev/null) 2>/dev/null&&exit 0;"
        + "[ -S $B/merge.sock ]&&kill -0 $(cat $B/merge.pid 2>/dev/null) 2>/dev/null&&kill -0 $(cat $B/websocket.pid 2>/dev/null) 2>/dev/null&&exit 0;"
        + "mkdir -p $B;killall vtouchd vtouchmerge vtouchws 2>/dev/null;rm -f $B/vtouchd.pid $B/merge.sock $B/merge.pid $B/websocket.pid;"
        + "if [ -x $D ];then nohup $D -w " + this.width + " -h " + this.height + " -p 27183 >$B/vtouchd.log 2>&1 </dev/null&echo $!>$B/vtouchd.pid;"
        + "else [ -x $M ]&&[ -x $W ]||{ echo binaries-missing;exit 11; };"
        + "nohup $M -s $B/merge.sock -v 10 -w " + this.width + " -h " + this.height + " >$B/merge.log 2>&1 </dev/null&echo $!>$B/merge.pid;"
        + "nohup $W >$B/websocket.log 2>&1 </dev/null&echo $!>$B/websocket.pid;fi";
    var r = shell(cmd, true);
    if (r && r.code !== 0) throw new Error("vtouch 启动失败: " + (r.error || r.result || ("code=" + r.code)));
    this.serviceStarted = true;
    return this;
};
VTouch.prototype.stopService = function () {
    try { shell("killall vtouchd vtouchmerge vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/vtouchd.pid /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid", true); } catch (e) { log("[vtouch] stop: " + e); }
    this.serviceStarted = false;
    return this;
};
VTouch.prototype.connect = function (onReady) {
    var self = this, startedAt = Date.now(), attempts = 0;
    if (onReady) this.onReady = onReady;
    if (this.opened) {
        if (this.onReady) { try { this.onReady(self); } catch (e) { log("[vtouch] onReady: " + e); } this.onReady = null; }
        return this;
    }
    if (this.connecting) return this;
    this.closed = false;
    this.connecting = true;
    try { this.startService(); } catch (e) { log("[vtouch] start: " + e); }
    (function attempt() {
        if (self.closed || self.opened) return;
        if (Date.now() - startedAt > self.timeout) { self.connecting = false; throw new Error("vtouch WS 连接超时"); }
        attempts++;
        var wd = setTimeout(function () { try { self.ws && self.ws.cancel(); } catch (e) {} }, 600);
        try { self.ws = new WebSocket(self.url); }
        catch (e) { clearTimeout(wd); self.retry = setTimeout(attempt, 100); return; }
        self.ws.on(WebSocket.EVENT_OPEN, function () {
            clearTimeout(wd);
            self.connecting = false;
            self.opened = true;
            while (self.queue.length) { try { self.ws.send(self.queue.shift()); } catch (e) { break; } }
            if (self.onReady) { try { self.onReady(self); } catch (e) { log("[vtouch] onReady: " + e); } self.onReady = null; }
        }).on(WebSocket.EVENT_TEXT, function (t) {
            t = String(t);
            if (t.indexOf("err") === 0) log("[vtouch] err: " + t);
        }).on(WebSocket.EVENT_CLOSED, function () {
            clearTimeout(wd); self.connecting = false; self.opened = false;
            if (!self.closed) self.retry = setTimeout(attempt, attempts <= 5 ? 50 : 100);
        }).on(WebSocket.EVENT_FAILURE, function (e) {
            clearTimeout(wd); self.connecting = false; self.opened = false; log("[vtouch] fail: " + e);
            if (!self.closed) self.retry = setTimeout(attempt, attempts <= 5 ? 50 : 100);
        });
    })();
    return this;
};
/* 同步等到 WS OPEN。内部 sleep，必须在业务线程调用。 */
VTouch.prototype.ready = function (timeout) {
    var t0 = Date.now(), lim = timeout || this.timeout;
    while (!this.opened) {
        if (this.closed || Date.now() - t0 > lim) throw new Error("vtouch 未就绪");
        sleep(50);
    }
    return this;
};
VTouch.prototype._send = function (s) {
    if (this.closed) throw new Error("vtouch 已关闭");
    if (this.opened) { if (!this.ws.send(s)) throw new Error("ws send 失败"); }
    else this.queue.push(s);
    return this;
};
VTouch.prototype.finger = function (slot) {
    var i;
    if (slot === undefined || slot === null) {
        for (i = 0; i <= 9; i++) if (!this.fingers[i] || !this.fingers[i].downState) break;
        if (i > 9) throw new Error("无空闲 slot");
        slot = i;
    } else {
        slot = Math.round(slot);
        if (slot < 0 || slot > 9) throw new Error("slot 0~9");
    }
    if (!this.fingers[slot]) this.fingers[slot] = new Finger(this, slot);
    return this.fingers[slot];
};
/* 原子多指帧：begin + point + end 连续发送，服务端一次 SYN_REPORT 提交。 */
VTouch.prototype.frame = function (pts) {
    var i, p;
    if (!pts || !pts.length) throw new Error("frame 不能为空");
    this._send("begin_frame");
    for (i = 0; i < pts.length; i++) {
        p = pts[i];
        if (p.x < 0 || p.y < 0 || p.x >= this.width || p.y >= this.height) { log("[vtouch] frame 越界跳过 " + p.x + "," + p.y); continue; }
        this._send("point " + p.slot + " " + p.state + " " + Math.round(p.x) + " " + Math.round(p.y));
        if (this.fingers[p.slot]) this.fingers[p.slot].downState = p.state !== "up";
    }
    this._send("end_frame");
    return this;
};
VTouch.prototype.reset = function () {
    var k;
    for (k in this.fingers) this.fingers[k].downState = false;
    return this._send("reset");
};
VTouch.prototype.close = function () {
    var k;
    if (this.opened && !this.closed) { try { this.ws.send("reset"); } catch (e) {} }
    for (k in this.fingers) this.fingers[k].downState = false;
    this.closed = true;
    this.opened = false;
    this.connecting = false;
    this.queue = [];
    if (this.retry) { try { clearTimeout(this.retry); } catch (e) {} }
    try { this.ws && this.ws.close(); } catch (e) { try { this.ws && this.ws.cancel(); } catch (e2) {} }
    return this;
};
var Finger = function (touch, slot) { this.touch = touch; this.slot = slot; this.downState = false; };
Finger.prototype.down = function (x, y) {
    this.touch._send("down " + this.slot + " " + Math.round(x) + " " + Math.round(y));
    this.downState = true;
    return this;
};
Finger.prototype.move = function (x, y) {
    if (!this.downState) return this;
    this.touch._send("move " + this.slot + " " + Math.round(x) + " " + Math.round(y));
    return this;
};
Finger.prototype.up = function () {
    if (!this.downState) return this;
    this.touch._send("up " + this.slot);
    this.downState = false;
    return this;
};
Finger.prototype.tap = function (x, y, ms) { this.down(x, y); sleep(ms == null ? 60 : ms); return this.up(); };
Finger.prototype.swipe = function (x1, y1, x2, y2, ms) {
    var total = ms == null ? 300 : ms, n = Math.max(2, Math.round(total / 16.7)), i;
    this.down(x1, y1);
    for (i = 1; i <= n; i++) { sleep(total / n); this.move(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n); }
    return this.up();
};
Finger.prototype.frame = function (state, x, y) { return this.touch.frame([{ slot: this.slot, state: state, x: x, y: y }]); };
Finger.prototype.state = function () { return this.downState ? "down" : "up"; };
