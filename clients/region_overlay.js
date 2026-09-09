// region_overlay.js — floaty 可视化：画区域框 + 实时触点。只管画，不管逻辑。
"use strict";
var _w = null, _regions = [], _fingers = [], _flash = {};
var Paint = android.graphics.Paint;
function _newPaint(color, style, width) {
    var p = new Paint();
    p.setColor(color); p.setStyle(style); p.setStrokeWidth(width); p.setTextSize(36);
    return p;
}
module.exports = {
    show: function (regions) {
        _regions = regions;
        if (_w) return _w;
        _w = floaty.rawWindow('<frame><canvas id="board" layout_weight="1"/></frame>');
        _w.setSize(device.width, device.height);
        _w.setTouchable(false);
        var self = this;
        _w.board.on("draw", function (canvas) {
            canvas.drawColor(0x00000000, android.graphics.PorterDuff.Mode.CLEAR);
            var i, r;
            for (i = 0; i < _regions.length; i++) {
                r = _regions[i];
                var hot = _flash[r.id] && Date.now() - _flash[r.id] < 400;
                var p = hot ? _newPaint(0xAA00FF00, Paint.Style.STROKE, 6)
                            : _newPaint(0x88FF0000, Paint.Style.STROKE, 3);
                canvas.drawRect(r.x1, r.y1, r.x2, r.y2, p);
                canvas.drawText(r.name || r.id, r.x1 + 8, r.y1 + 40, _newPaint(0xDDFFFFFF, Paint.Style.FILL, 2));
            }
            for (i = 0; i < _fingers.length; i++) {
                var f = _fingers[i];
                canvas.drawCircle(f.x, f.y, 40, _newPaint(0xAA00B0FF, Paint.Style.FILL, 2));
                canvas.drawText("s" + f.slot, f.x + 44, f.y, _newPaint(0xDDFFFFFF, Paint.Style.FILL, 2));
            }
        });
        return _w;
    },
    update: function (fingers) { _fingers = fingers || []; },
    flash: function (id) { _flash[id] = Date.now(); },
    setRegions: function (rs) { _regions = rs; },
    close: function () { try { if (_w) _w.close(); } catch (e) {} _w = null; }
};
