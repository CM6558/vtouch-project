/* =============================================================
 * vtouch AutoJs6 SDK — 同步式 API（精简版）
 *
 * 同步式写法（必须放进 vt.run() 子线程，主线程保持空闲处理 WebSocket 事件）：
 *   var vt = new VTouch();
 *   vt.run(function () {
 *       vt.connect();                       // 阻塞直到连接就绪（自动启动服务/快速重试/超时）
 *       var f = vt.finger();
 *       f.swipe(200, 200, 1500, 2000, 1000); // 同步：内部按帧 sleep 插值
 *       f.tap(720, 1584, 60);               // 同步：按下 - sleep - 抬起
 *       vt.stop();                           // 关闭连接并停止服务（释放 EVIOCGRAB）
 *   });
 *
 * 连接：服务未运行则自动启动（幂等）；失败 300ms 快速重试直到超时。
 * 坐标：device.width/height 逻辑坐标，服务端换算到原始触摸轴。
 * 生命周期：业务完成后调用 vt.stop()，脚本随之自然结束，避免旧连接占用
 *           单连接的 vtouchws 导致下次连接失败。
 * ============================================================= */
"use strict";

function Finger(t, s) { this.touch = t; this.slot = s; this.active = false; }

Finger.prototype._p = function (x, y) {
    x = Math.round(x); y = Math.round(y);
    if (x < 0 || x >= this.touch.width || y < 0 || y >= this.touch.height) return null;
    return { x: x, y: y };
};
Finger.prototype.down = function (x, y) {
    var p = this._p(x, y);
    if (p) { this.touch._send('down ' + this.slot + ' ' + p.x + ' ' + p.y); this.active = true; }
    else log('[vtouch] oob down');
    return this;
};
Finger.prototype.move = function (x, y) {
    if (!this.active) { log('[vtouch] not down'); return this; }
    var p = this._p(x, y);
    if (p) this.touch._send('move ' + this.slot + ' ' + p.x + ' ' + p.y);
    return this;
};
Finger.prototype.up = function () {
    if (this.active) { this.active = false; this.touch._send('up ' + this.slot); }
    return this;
};
Finger.prototype.tap = function (x, y, ms) {       /* 同步：按下-sleep-抬起 */
    ms = ms == null ? 60 : ms;
    this.down(x, y); sleep(ms); this.up();
    return this;
};
Finger.prototype.swipe = function (x1, y1, x2, y2, ms) {  /* 同步：按帧 sleep 插值 */
    ms = ms == null ? 300 : ms;
    var n = Math.max(2, Math.round(ms / 16)), i;
    this.down(x1, y1);
    for (i = 1; i <= n; i++) {
        sleep(Math.max(1, ms / n));
        if (!this.active) this.down(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n);
        else this.move(x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n);
    }
    this.up();
    return this;
};
Finger.prototype.frame = function (st, x, y) { return this.touch.frame([{ slot: this.slot, state: st, x: x, y: y }]); };
Finger.prototype.state = function () { return this.active ? 'down' : 'up'; };

var VTouch = function (o) {
    o = o || {};
    this.url = o.url || 'ws://127.0.0.1:27183';
    this.width = o.width || device.width;
    this.height = o.height || device.height;
    this.timeout = o.timeout || 15000;
    this.onError = o.onError || null;
    this.ws = null; this.opened = false; this._s = false;
    this.q = []; this.fingers = {};
};

/* 幂等启动服务：已运行则秒退，否则拉起（只真正执行一次） */
VTouch.prototype.start = function () {
    if (this._s) return this;
    this._s = true;
    var B = '/data/local/tmp/vtouch-runtime',
        c = 'B=' + B + ';M=/data/local/tmp/vtouchmerge;W=/data/local/tmp/vtouchws;'
          + '[ -x $M ]&&[ -x $W ]||{ echo vtouch-bin-missing;exit 11; };mkdir -p $B;'
          + 'if [ -S $B/merge.sock ]&&[ -f $B/websocket.pid ]&&kill -0 $(cat $B/websocket.pid) 2>/dev/null;then exit 0;fi;'
          + 'killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f $B/merge.sock $B/merge.pid $B/websocket.pid;'
          + 'nohup $M -s $B/merge.sock -v 10 -w ' + this.width + ' -h ' + this.height + ' >/dev/null 2>&1 </dev/null &echo $!>$B/merge.pid;'
          + 'nohup $W >/dev/null 2>&1 </dev/null &echo $!>$B/websocket.pid';
    var r = shell(c, true);
    if (r && r.code !== 0) throw new Error('vtouch start failed: ' + (r.error || r.result || ''));
    return this;
};

/* 同步连接：阻塞到就绪或超时（须在 vt.run 子线程调用，主线程处理事件） */
VTouch.prototype.connect = function () {
    var self = this;
    if (this.opened) return this;
    this.start();
    var t0 = Date.now();
    function tryOnce() {
        if (self.opened) return;
        try {
            self.ws = new WebSocket(self.url);
            self.ws.on(WebSocket.EVENT_OPEN, function () { self.opened = true; self._flush(); })
                .on(WebSocket.EVENT_TEXT, function (t) { t = String(t); if (t.slice(0, 3) === 'err' && self.onError) self.onError(t); })
                .on(WebSocket.EVENT_CLOSED, function () { self.opened = false; })
                .on(WebSocket.EVENT_FAILURE, function () { self.opened = false; });
        } catch (e) { self.opened = false; }
    }
    tryOnce();
    while (!this.opened && Date.now() - t0 < this.timeout) {
        sleep(300);
        if (!this.opened) { try { this.ws && this.ws.cancel(); } catch (e) {} tryOnce(); }
    }
    if (!this.opened) throw new Error('vtouch connect timeout');
    return this;
};

/* 在子线程运行业务（同步式写法入口，主线程保持空闲） */
VTouch.prototype.run = function (fn) { threads.start(fn); return this; };

VTouch.prototype._flush = function () {
    var m;
    while (this.opened && this.q.length) {
        m = this.q.shift();
        try { if (!this.ws.send(m)) break; } catch (e) { this.close(); break; }
    }
};
VTouch.prototype._send = function (m) { this.q.push(m); if (this.opened) this._flush(); };

VTouch.prototype.finger = function (s) {
    var i;
    if (s == null) {
        for (i = 0; i <= 9; i++) if (!this.fingers[i] || this.fingers[i].state() === 'up') break;
        if (i > 9) throw new Error('no free slot');
        s = i;
    } else { s = Math.round(s); if (s < 0 || s > 9) throw new Error('slot 0~9'); }
    if (!this.fingers[s]) this.fingers[s] = new Finger(this, s);
    return this.fingers[s];
};
VTouch.prototype.frame = function (pts) {
    var i, o, s, p, v = [];
    if (!pts || !pts.length) throw new Error('frame empty');
    for (i = 0; i < pts.length; i++) {
        o = pts[i];
        if (!o || ['down', 'move', 'up'].indexOf(o.state) < 0) throw new Error('bad state');
        s = Math.round(o.slot);
        p = { x: Math.round(o.x), y: Math.round(o.y) };
        if (p.x < 0 || p.x >= this.width || p.y < 0 || p.y >= this.height) { log('[vtouch] oob'); continue; }
        v.push({ slot: s, state: o.state, x: p.x, y: p.y });
    }
    if (!v.length) return this;
    this._send('begin_frame');
    for (i = 0; i < v.length; i++) {
        this._send('point ' + v[i].slot + ' ' + v[i].state + ' ' + v[i].x + ' ' + v[i].y);
        if (this.fingers[v[i].slot]) {
            if (v[i].state === 'down') this.fingers[v[i].slot].active = true;
            else if (v[i].state === 'up') this.fingers[v[i].slot].active = false;
        }
    }
    this._send('end_frame');
    return this;
};
VTouch.prototype.reset = function () { for (var k in this.fingers) this.fingers[k].active = false; this._send('reset'); return this; };
VTouch.prototype.close = function () {
    this.opened = false;
    try { this.ws && this.ws.close(WebSocket.CODE_CLOSE_NORMAL, 'exit'); } catch (e) {}
    return this;
};
VTouch.prototype.stop = function () {
    this.close();
    try { shell('killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid', true); } catch (e) {}
    return this;
};
VTouch.Finger = Finger;