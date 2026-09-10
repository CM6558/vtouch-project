/* vtouch_uibtn.js — 远程点按钮：收起键/最小化键/悬浮球恢复，全走真实点击路径。
 * 收起断言：body GONE + 底部按钮位置上移（窗体收小）；最小化断言：球在且窗关；点球恢复。
 */
var vt = require("/sdcard/vtouch_bundle.js");
try { events.broadcast.emit("vt-takeover", "uibtn_" + Date.now()); } catch (e) {}
sleep(1500);
vt.ensure();
var c = vt.connect();
eval(vt.uiSource);
function chk(n, cnd, ex) { if (cnd) log("PASS " + n); else log("FAIL " + n + " " + (ex || "")); }
function qpos(v) {
    var loc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
    v.getLocationOnScreen(loc);
    return { x: loc[0], y: loc[1] };
}
try {
    uiOnMain(function () { ui(); });
    sleep(800);
    var yFull = 0, yFold = 0, yBack = 0;
    uiOnMain(function () { yFull = qpos(g_uiW.btnQuit).y; g_uiW.btnFold.performClick(); });
    sleep(500);
    uiOnMain(function () {
        chk("fold-vis", g_uiW.body.getVisibility() === 8, "vis=" + g_uiW.body.getVisibility());
        yFold = qpos(g_uiW.btnQuit).y;
    });
    chk("fold-shrink", yFold < yFull - 200, "quitY " + yFold + " vs " + yFull);
    uiOnMain(function () { g_uiW.btnFold.performClick(); });
    sleep(500);
    uiOnMain(function () { yBack = qpos(g_uiW.btnQuit).y; });
    chk("unfold-grow", Math.abs(yBack - yFull) < 60, "quitY " + yBack + " vs " + yFull);
    uiOnMain(function () { g_uiW.btnMin.performClick(); });
    sleep(600);
    chk("min-ball", !g_uiW && !!g_minW, "");
    var bx = 0, by = 0;
    uiOnMain(function () {
        var p = qpos(g_minW.ball);
        bx = p.x + g_minW.ball.getWidth() / 2; by = p.y + g_minW.ball.getHeight() / 2;
    });
    log("BALL at " + Math.round(bx) + "," + Math.round(by));
    vt.finger().tap(bx, by);
    sleep(800);
    chk("ball-restore", !!g_uiW && !g_minW, "");
    uiOnMain(function () { uiClose(); ovClose(); });
    sleep(300);
    chk("uiclose", !g_uiW && !g_minW, "");
} catch (e) { log("UIBTN-ERR " + e); }
try { c.close(); } catch (e2) {}
try { vt.stop(); } catch (e3) {}
log("DONE-UIBTN");
exit();
