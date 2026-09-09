#!/usr/bin/env node
// SDK 冒烟：用桩跑通 clients/plugins/vtouch.js 的 connect 重试状态机。
// 覆盖：构造抛错不逃逸、首次 FAILURE 后重试、二次 OPEN 后刷新队列 + onReady。
"use strict";
const path = require("path");

const handlers = [];
let instances = 0;

global.device = { width: 1440, height: 3168 };
global.shell = () => ({ code: 0, result: "", error: "" });
global.log = () => {};
global.sleep = () => {};
global.events = { on: () => {} };

function FakeWS(url) {
    instances++;
    this.url = url;
    this.map = {};
    this.sent = [];
    handlers.push(this);
}
FakeWS.EVENT_OPEN = "open";
FakeWS.EVENT_TEXT = "text";
FakeWS.EVENT_CLOSED = "closed";
FakeWS.EVENT_FAILURE = "failure";
FakeWS.CODE_CLOSE_NORMAL = 1000;
FakeWS.prototype.on = function (ev, fn) { this.map[ev] = fn; return this; };
FakeWS.prototype.send = function (s) { this.sent.push(s); return true; };
FakeWS.prototype.close = function () {};
FakeWS.prototype.cancel = function () { if (this.map.closed) this.map.closed(1006, "cancel"); };
global.WebSocket = FakeWS;

const VTouch = require(path.join(__dirname, "..", "clients", "plugins", "vtouch.js"));

let readyCalls = 0;
const vt = new VTouch({ timeout: 5000 });
vt.connect(() => { readyCalls++; });

function tick(ms) { return new Promise((r) => setTimeout(r, ms)); }

(async () => {
    await tick(50);
    if (instances < 1) throw new Error("no WS attempt made");
    // 首次失败：必须触发重试（新实例出现），且不能抛 ReferenceError 导致静默死亡
    handlers[0].map.failure("ECONNREFUSED");
    await tick(300);
    if (instances < 2) throw new Error("no retry after FAILURE (retry chain dead?)");
    // 二次成功：开门 + 冲队列 + 回调一次
    vt._send("ping");
    handlers[1].map.open();
    await tick(50);
    if (!vt.opened) throw new Error("not opened after OPEN");
    if (readyCalls !== 1) throw new Error("onReady calls=" + readyCalls);
    if (handlers[1].sent.join("").indexOf("ping") < 0) throw new Error("queue not flushed");
    // 关闭后不再重试
    const n = instances;
    vt.close();
    await tick(300);
    if (instances !== n) throw new Error("retry after close");
    console.log("SMOKE_OK instances=" + instances + " version=" + (VTouch.VERSION || "?"));
})().catch((e) => { console.error("SMOKE_FAIL " + (e && e.stack || e)); process.exit(1); });
