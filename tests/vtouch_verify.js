/* vtouch_verify.js — 上机自测（竖屏/横屏各跑一遍）：
 * 1) 注入闭环：自建全屏触摸窗，vt.finger().tap() 打进去，窗内 onTouch 回读坐标，差值<12px 算过。
 *    CAP-READY 后 PC 端可补一发 `adb shell input tap` 做框架基准对照（看 CAP-GOT 行）。
 * 2) UI 断言：开窗/收起/展开/回顶/最小化球/恢复/关闭。
 * 跑完自动清场（关窗/reset/stop），旧 watcher 会被开头的接管广播请退。
 * 注意：调用文件禁 "use strict"（eval(uiSource) 要进主上下文）。
 */
var vt = require("/sdcard/vtouch_bundle.js");
try { events.broadcast.emit("vt-takeover", "verify_" + Date.now()); } catch (e) {}
sleep(1500);
var T = { ok: 0, fail: 0 };
function chk(name, cond, extra) {
    if (cond) { T.ok++; log("PASS " + name); }
    else { T.fail++; log("FAIL " + name + " " + (extra || "")); }
}
vt.ensure();
var c = vt.connect();
eval(vt.uiSource);
try {
    log("ROT=" + vt.rot() + " WH=" + device.width + "x" + device.height);
    /* ---- 注入闭环 ---- */
    var got = null;
    var w = floaty.rawWindow('<frame id="cap"><canvas id="board" layout_weight="1"/></frame>');
    w.setSize(device.width, device.height);
    w.setTouchable(true);
    w.cap.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, { onTouch: function (v, ev) {
        try {
            if (ev.getAction() === 0) {
                var loc = java.lang.reflect.Array.newInstance(java.lang.Integer.TYPE, 2);
                w.cap.getLocationOnScreen(loc);
                got = { x: ev.getX() + loc[0], y: ev.getY() + loc[1] };
                log("CAP-GOT " + Math.round(got.x) + "," + Math.round(got.y));
            }
        } catch (e) {}
        return true;
    } }));
    sleep(500);
    log("CAP-READY");
    sleep(8000);
    var pts = [[200, 400], [device.width - 200, device.height - 300]];
    for (var i = 0; i < pts.length; i++) {
        got = null;
        vt.finger().tap(pts[i][0], pts[i][1]);
        sleep(600);
        if (got) {
            var dx = Math.abs(got.x - pts[i][0]), dy = Math.abs(got.y - pts[i][1]);
            chk("tap" + i, dx < 12 && dy < 12, "want " + pts[i][0] + "," + pts[i][1] + " got " + Math.round(got.x) + "," + Math.round(got.y));
        } else chk("tap" + i, false, "no touch received for " + pts[i][0] + "," + pts[i][1]);
    }
    try { w.close(); } catch (e) {}
    /* ---- UI 断言（写 view 走主线程） ---- */
    uiOnMain(function () { ui(); });
    sleep(800);
    chk("ui-open", !!g_uiW, "");
    uiOnMain(function () { g_uiFolded = true; uiApplyFold(); });
    sleep(300);
    chk("fold", g_uiW.body.getVisibility() === 8, "vis=" + g_uiW.body.getVisibility());
    uiOnMain(function () { g_uiFolded = false; uiApplyFold(); });
    sleep(300);
    chk("unfold", g_uiW.body.getVisibility() === 0, "vis=" + g_uiW.body.getVisibility());
    uiOnMain(function () { ovShow(vt.loadRegions()); });
    var ovBefore = g_ovW;
    uiOnMain(function () { ovRetop(); });
    chk("retop", !!g_ovW && g_ovW !== ovBefore, "");
    uiOnMain(function () { uiMin(); });
    sleep(600);
    chk("min", !g_uiW && !!g_minW, "");
    uiOnMain(function () { uiMinClose(); ui(); });
    sleep(600);
    chk("restore", !!g_uiW && !g_minW, "");
    uiOnMain(function () { uiClose(); ovClose(); });
    sleep(300);
    chk("uiclose", !g_uiW && !g_minW && !g_ovW, "");
} catch (e) { log("VERIFY-ERR " + e + "\n" + e.stack); }
try { c.close(); } catch (e2) {}
try { vt.reset(); } catch (e3) {}
try { vt.stop(); } catch (e4) {}
log("DONE ok=" + T.ok + " fail=" + T.fail);
exit();
