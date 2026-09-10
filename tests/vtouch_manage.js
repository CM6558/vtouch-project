/* vtouch_manage.js — 带管理 UI 的启动器：监听 + overlay + 直接打开区域管理窗。
 * 跑起来后：红色框=区域，蓝色点=手指；管理窗可拖动/收起/最小化，＋矩形/＋圆形框选。
 */
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);

(function () {
    var rs = vt.loadRegions(), i, hasCircle = false;
    for (i = 0; i < rs.length; i++) if (rs[i].type === "circle") hasCircle = true;
    if (!hasCircle) {
        rs.push({ id: "c0", name: "圆形区", type: "circle", cx: 720, cy: 2400, r: 220, enabled: true });
        vt.rgSave(rs);
    }
})();

bootWatch({
    onDown: function (region, f) { ovFlash(region.id); toast("按下 " + (region.name || region.id)); },
    onUp: function (region, f) { log("ev up " + region.id + " s" + f.slot); },
    onEnter: function (region, f) { ovFlash(region.id); log("ev enter " + region.id + " s" + f.slot); },
    onExit: function (region, f) { log("ev exit " + region.id + " s" + f.slot); },
    onMove: function (region, f) {}
});

ui();
