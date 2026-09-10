/* vtouch_dbg.js — 分步落盘定位启动失败（绕开 logcat 限流）。看 /sdcard/vtouch_dbg.log */
function rec(s) {
    try { files.write("/sdcard/vtouch_dbg.log", s + "\n"); } catch (e) {}
    try { log(s); } catch (e2) {}
}
try {
    rec("1 require...");
    var vt = require("/sdcard/vtouch_bundle.js");
    rec("2 takeover...");
    try { events.broadcast.emit("vt-takeover", "dbg_" + Date.now()); } catch (e) { rec("2b bc-err " + e); }
    sleep(1500);
    rec("3 ensure...");
    vt.ensure();
    rec("4 connect...");
    var c = vt.connect();
    rec("5 eval...");
    eval(vt.uiSource);
    rec("6 bootWatch...");
    bootWatch({ onDown: function (r, f) {}, onUp: function (r, f) {}, onEnter: function (r, f) {}, onExit: function (r, f) {}, onMove: function (r, f) {} });
    rec("7 ui...");
    ui();
    rec("8 ALIVE");
} catch (e) { rec("FAIL " + e); }
