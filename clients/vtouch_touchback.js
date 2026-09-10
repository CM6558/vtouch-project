/* 监听 + 回触示例：物理手指触发区域 → 虚拟手指自动执行触摸动作。
 * 左区按下 → 上滑一次；右区按下 → 点一次。坐标按竖屏写，横屏自动换算。
 * 注意两点：
 * 1) 回调跑在订阅读线程，耗时动作（swipe 里有 sleep）必须丢进 threads.start，
 *    否则读线程卡住，事件全堵。
 * 2) 虚拟触摸不产生 region_ev（daemon 只匹配物理槽），不会自激，放心连击。
 * 管理：ui() 开/关；框选新区默认只有监听，想让它回触按下面格式加分支。
 */
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);

(function () {
    var rs = vt.loadRegions(), i, hasL = false, hasR = false;
    for (i = 0; i < rs.length; i++) {
        if (rs[i].id === "swipeL") hasL = true;
        if (rs[i].id === "tapR") hasR = true;
    }
    if (!hasL) rs.push({ id: "swipeL", name: "左滑区", x1: 60, y1: 2200, x2: 660, y2: 2900, enabled: true });
    if (!hasR) rs.push({ id: "tapR", name: "右点区", x1: 780, y1: 2200, x2: 1380, y2: 2900, enabled: true });
    if (!hasL || !hasR) vt.rgSave(rs);
})();

/* 回触统一走子线程：读线程只负责收事件。 */
function busy(fn) {
    threads.start(function () {
        try { fn(); } catch (e) { toast("回触失败: " + e); }
    });
}

bootWatch({
    onDown: function (region, f) {
        ovFlash(region.id);
        if (region.id === "swipeL") {
            toast("上滑一下");
            busy(function () { vt.finger().swipe(720, 2400, 720, 1200, 350); });
        } else if (region.id === "tapR") {
            toast("点一下");
            busy(function () { vt.finger().tap(720, 1500); });
        } else {
            toast("按下 " + (region.name || region.id));
        }
    },
    onUp: function (region, f) {},
    onEnter: function (region, f) {},
    onExit: function (region, f) {},
    onMove: function (region, f) {}
});

ui();  // 管理区域：列表/开关/显隐/删除，＋矩形/圆形框选
