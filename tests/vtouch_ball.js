/* vtouch_ball.js — 复现最小化→点球恢复，抓 CalledFromWrongThread 现行 */
var vt = require("/sdcard/vtouch_bundle.js");
try { events.broadcast.emit("vt-takeover", "ball_" + Date.now()); } catch (e) {}
sleep(1500);
vt.ensure();
var c = vt.connect();
eval(vt.uiSource);
function chk(n, cnd, ex) { if (cnd) log("PASS " + n); else log("FAIL " + n + " " + (ex || "")); }
try {
    uiOnMain(function () { ui(); });
    sleep(800);
    uiOnMain(function () { g_uiW.btnMin.performClick(); });
    sleep(800);
    chk("min-ball", !g_uiW && !!g_minW, "");
    var bx = 0, by = 0;
    uiOnMain(function () {
        var loc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
        g_minW.ball.getLocationOnScreen(loc);
        bx = loc[0] + g_minW.ball.getWidth() / 2; by = loc[1] + g_minW.ball.getHeight() / 2;
    });
    log("BALL at " + Math.round(bx) + "," + Math.round(by));
    vt.finger().tap(bx, by);
    sleep(1000);
    chk("ball-restore", !!g_uiW && !g_minW, "");
    uiOnMain(function () { uiClose(); ovClose(); });
} catch (e) { log("BALL-ERR " + e + " || " + (e.stack || "")); }
try { c.close(); } catch (e2) {}
try { vt.stop(); } catch (e3) {}
log("DONE-BALL");
exit();
