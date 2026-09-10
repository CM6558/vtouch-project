/* 触摸业务：只写触发后干什么。其它（接管/启动/订阅/线程/退出/保活/overlay/UI）全在 bundle 里。
 * 管理：ui() 开/关；预览：ovPreview(true/false)。
 */
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);

/* 种子圆形区：库里没有圆形才加 */
(function () {
    var rs = vt.loadRegions(), i, hasCircle = false;
    for (i = 0; i < rs.length; i++) if (rs[i].type === "circle") hasCircle = true;
    if (!hasCircle) {
        rs.push({ id: "c0", name: "圆形区", type: "circle", cx: 720, cy: 2400, r: 220, enabled: true });
        vt.rgSave(rs);
    }
})();

var moveCount = 0;
bootWatch({
    onDown: function (region, f) {
        ovFlash(region.id);
        toast("按下 " + (region.name || region.id));
        log("ev down " + region.id + " s" + f.slot + " " + Math.round(f.x) + "," + Math.round(f.y));
    },
    onMove: function (region, f) {
        moveCount++;
        if (moveCount % 15 === 1) log("ev move " + region.id + " s" + f.slot + " " + Math.round(f.x) + "," + Math.round(f.y));
    },
    onUp: function (region, f) {
        log("ev up " + region.id + " s" + f.slot);
    },
    onEnter: function (region, f) {
        ovFlash(region.id);
        log("ev enter " + region.id + " s" + f.slot);
    },
    onExit: function (region, f) {
        log("ev exit " + region.id + " s" + f.slot);
    }
});

// ui();  // 管理区域：列表/开关/显隐/删除，＋矩形/圆形框选，预览开关
