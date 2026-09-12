/* vtouch_touchback.js — 回触示例（当前架构：单文件启动，面板自身就是 daemon）
 *
 * 依赖只有一个文件：/sdcard/vtouch_bundle.js
 *   —— 面板 dex+so、vtouchd 二进制都内嵌在里面，首次运行自动释放到设备侧，不用任何 .sh。
 *
 * 链路：物理手指 → 面板 grab 后推 pev → 本脚本订阅 → 命中区域 → 回触（另一路虚拟槽注入）
 *
 * 跑法：AutoJs6 里直接跑本文件。面板区域可在面板里改（改完自动存 regions.conf，重启还在），
 *   本脚本只补两个缺省区：左滑区 / 右点区。
 *
 * 两个坑（都踩过）：
 * 1) 回调跑在订阅读线程：带 sleep 的动作（swipe/tap 内部有 sleep）必须 threads.start，
 *    否则读线程卡住，之后所有事件全堵。
 * 2) 虚拟触摸不产生 pev（daemon 只报物理槽），所以回触不会自激连击。
 */
var vt = require("/sdcard/vtouch_bundle.js");

/* 面板 = UI + daemon：full 模式 grab 物理触摸 + 开 WS 27183 + 读 regions.conf。
 * 已经在跑就复用（不重启）；退出时 vt.stop() 会连面板一起收，释放 grab。 */
vt.uiStart();

var c = vt.connect();

/* 区域唯一归属是面板：先回读面板当前表（rgList 要在收包循环之前调），只补缺的两个，
 * 再整表下发（rgPush 里先 region clear）。面板改的区域下次仍在，不会被脚本抹掉。 */
var rs = vt.rgList(c), i, hasL = false, hasR = false;
for (i = 0; i < rs.length; i++) {
    if (rs[i].id === "swipeL") hasL = true;
    if (rs[i].id === "tapR") hasR = true;
}
if (!hasL) rs.push({ id: "swipeL", name: "左滑区", x1: 60, y1: 2200, x2: 660, y2: 2900, enabled: true });
if (!hasR) rs.push({ id: "tapR", name: "右点区", x1: 780, y1: 2200, x2: 1380, y2: 2900, enabled: true });
vt.rgPush(c, rs);

/* 回触统一丢子线程：读线程只负责收事件 */
function busy(fn) {
    threads.start(function () { try { fn(); } catch (e) { toastLog("回触失败: " + e); } });
}

var eng = vt.createEngine(rs, {
    onDown: function (r) {
        if (r.id === "swipeL") {
            toast("上滑一下");
            busy(function () { vt.finger().swipe(720, 2400, 720, 1200, 350); });
        } else if (r.id === "tapR") {
            toast("点一下");
            busy(function () { vt.finger().tap(720, 1500); });
        } else {
            toast("按下 " + (r.name || r.id));
        }
    },
    onUp: function (r) {}, onEnter: function (r) {}, onExit: function (r) {}, onMove: function (r) {}
});

/* 想让「哪个区域被触发」由 daemon 直接告诉你（不用在 JS 里存区域表、重算命中），
 * 就读 region_ev 行：region_ev <id> <down|up|enter|exit|move> <slot> <lx> <ly>
 *   id = 区域（面板里可改名改成的那个 id），slot = 哪根手指/哪个触点。
 * 下面这行就是全部分发逻辑的入口： */
function onRegion(h) {
    if (h.ev !== "down") return;
    toast("区域 " + h.id + " 被 slot" + h.slot + " 按下 @" + h.x + "," + h.y);
}

vt.sub(c);   /* 订阅物理触摸流：此后服务端推 pev 行（订阅后 region_ev 行也会一起来） */
threads.start(function () {   /* 读线程常驻；recv() 非阻塞，空转 sleep 让一下 */
    for (;;) {
        var line = null;
        try { line = c.recv(); } catch (e) { break; }
        if (line) {
            var ev = vt.parseEv(line), rg = vt.rgParseEv(line);
            if (ev) eng.feed(ev);            /* 手指事件：走引擎的本地命中判定 */
            if (rg) onRegion(rg);            /* 区域事件：daemon 直接给了区域 id */
        }
        else sleep(8);
    }
});

/* 收尾不用自己写：第一次 connect() 时库就注册了退出钩子，脚本结束自动 vt.stop()
 * （收面板 + 释放 EVIOCGRAB）。要故意留面板用 vt.autoStop(false)。 */

toastLog("回触已就绪（面板在跑）：左滑区 / 右点区 各按一下试试");
