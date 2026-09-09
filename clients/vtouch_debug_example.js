"use strict";
// 对比：Java 裸 Socket（status）vs OkHttp WebSocket，同进程同目标。
var VTouch = plugins.load('org.vtouch.plugin');
function mark(s) { try { files.append("/sdcard/ex-debug.txt", Date.now() + " " + s + "\n"); } catch (e) {} }
var vt = new VTouch();
mark("status=" + vt.status());
var ws = null;
try {
    ws = new WebSocket("ws://127.0.0.1:27183");
    mark("ws constructed");
} catch (e) { mark("ws construct throw " + e); }
if (ws) {
    ws.on(WebSocket.EVENT_OPEN, function () { mark("ws OPEN"); try { ws.send("ping"); } catch (e) { mark("send throw " + e); } })
      .on(WebSocket.EVENT_TEXT, function (t) { mark("ws TEXT " + t); })
      .on(WebSocket.EVENT_FAILURE, function (e) { mark("ws FAILURE " + e); })
      .on(WebSocket.EVENT_CLOSED, function (c, r) { mark("ws CLOSED " + c); });
}
var t0 = Date.now();
while (Date.now() - t0 < 8000) sleep(500);
mark("end");
try { ws.cancel(); } catch (e) {}
vt.close();
exit();
