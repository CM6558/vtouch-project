"use strict";
// 最小 WS 探针：只连 127.0.0.1:27183，记录全部事件，结果写 /sdcard/ws-probe.txt
var out = [];
function w(s) { out.push(s); log("[wsprobe] " + s); }
function dump() { try { files.write("/sdcard/ws-probe.txt", out.join("\n")); } catch (e) {} }
var ws = null;
try {
    ws = new WebSocket("ws://127.0.0.1:27183");
    w("constructed");
} catch (e) {
    w("construct throw: " + e);
    dump();
    exit();
}
ws.on(WebSocket.EVENT_OPEN, function () {
    w("OPEN");
    try { ws.send("ping"); w("sent ping"); } catch (e) { w("send throw: " + e); }
}).on(WebSocket.EVENT_TEXT, function (t) {
    w("TEXT " + t);
    dump();
    try { ws.close(); } catch (e) {}
}).on(WebSocket.EVENT_FAILURE, function (e) {
    w("FAILURE " + e);
}).on(WebSocket.EVENT_CLOSED, function (c, r) {
    w("CLOSED " + c + " " + r);
});
setTimeout(function () {
    w("TIMEOUT 8s");
    dump();
    try { ws.cancel(); } catch (e) {}
    exit();
}, 8000);
