/* vtouch_orient_demo.js — 方向换算复核（当前架构：面板 + 单文件依赖）
 *
 * 依赖只有 /sdcard/vtouch_bundle.js。
 * 测什么：面板 grab 后推给脚本的 pev 是**逻辑**坐标（面板已按 rotation 换算），
 *   本脚本在四角各挂一个区域，你在某个角点一下 → 事件若命中「你点的那个角」，换算就是对的；
 *   转屏后四角按新的 device.width/height 重算，再点一次即可复核另一个方向。
 *
 * 注入侧落点（虚拟触摸打到屏幕上哪儿）脚本自证不了：虚拟触摸走 uinput，不产生 pev。
 *   想肉眼复核就配合 vtouch_touchback.js：回触的滑动/点击落点用目标 App 观察。
 *
 * 结论写到 /sdcard/vtouch_orient.out，也 toast 出来。
 */
var vt = require("/sdcard/vtouch_bundle.js");
var OUT = "/sdcard/vtouch_orient.out";
var L = [];
function say(s) { L.push(s); try { files.write(OUT, L.join("\n")); } catch (e) {} log(s); toast(s); }

vt.uiStart();
var c = vt.connect();

var NAMES = ["左上", "右上", "左下", "右下"];
function cornerRegions() {
    var w = device.width, h = device.height, s = Math.round(Math.min(w, h) * 0.14);
    var pts = [[0, 0], [w - s, 0], [0, h - s], [w - s, h - s]], out = [], i;
    for (i = 0; i < 4; i++) {
        out.push({ id: "corner" + i, name: NAMES[i] + "角", x1: pts[i][0], y1: pts[i][1], x2: pts[i][0] + s, y2: pts[i][1] + s, enabled: true });
    }
    return out;
}

var CORNERS = cornerRegions();
vt.rgPush(c, CORNERS);   /* 四角也下发到面板，面板上看到的跟脚本判定的是同一份 */
var eng = vt.createEngine(CORNERS, {
    onDown: function (r, f) {
        say("命中 " + (r.name) + "  逻辑坐标 " + Math.round(f.x) + "," + Math.round(f.y)
            + "  （点的是哪个角？对上就是换算正确）");
    }
});

vt.sub(c);
threads.start(function () {
    for (;;) {
        var line = null;
        try { line = c.recv(); } catch (e) { break; }
        if (line) { var ev = vt.parseEv(line); if (ev) eng.feed(ev); }
        else sleep(8);
    }
});

/* 转屏即换一套四角：拖到新方向再点一次。 */
var last = -1;
setInterval(function () {
    try {
        var r = vt.rot();
        if (r !== last) {
            if (last !== -1) { CORNERS = cornerRegions(); vt.rgPush(c, CORNERS); eng.setRegions(CORNERS); say("转屏 rotation=" + r + "，四角已重算，请再点一次"); }
            last = r;
        }
    } catch (e) {}
}, 1000);

events.on("exit", function () { try { c.close(); } catch (e) {} vt.stop(); });
say("四角区域就绪（rotation=" + vt.rot() + "）：点一下某个角看看命中对不对");
