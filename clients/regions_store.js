// regions_store.js — 区域存取，ES5。UI 框选页和管理页写同一份，引擎只读。
"use strict";
var _store = storages.create("vtouch_regions");
function _def() {
    return [{ id: "btn", name: "按钮区", x1: 400, y1: 1000, x2: 1040, y2: 1400, enabled: true }];
}
module.exports = {
    loadRegions: function () {
        try {
            var rs = _store.get("regions");
            if (rs && rs.length) return rs;
        } catch (e) {}
        return _def();
    },
    saveRegions: function (rs) { _store.put("regions", rs); },
    addRegion: function (r) {
        var rs = this.loadRegions(); rs.push(r); this.saveRegions(rs); return rs;
    }
};
