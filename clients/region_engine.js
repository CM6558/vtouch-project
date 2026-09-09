// region_engine.js — 纯函数，无 IO 无 sleep，可直接单测。输入点流，输出区域事件。
// 点: {slot, x, y, action} action ∈ down/move/up ；区域: {id,x1,y1,x2,y2,enabled}
"use strict";
function inRect(r, x, y) {
    return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
}
function createEngine(regions, handlers) {
    var inside = {}; // slot -> {regionId: true}
    var fingers = {}; // slot -> {slot,x,y,down}
    var eng = {
        fingers: function () {
            var out = [], k;
            for (k in fingers) out.push(fingers[k]);
            return out;
        },
        setRegions: function (rs) { regions = rs; },
        feed: function (p) {
            var evts = [], i, r, key, f;
            f = fingers[p.slot] || (fingers[p.slot] = { slot: p.slot, x: 0, y: 0, down: false });
            f.x = p.x; f.y = p.y;
            if (p.action === "down") f.down = true;
            if (p.action === "up") f.down = false;
            if (!inside[p.slot]) inside[p.slot] = {};
            for (i = 0; i < regions.length; i++) {
                r = regions[i];
                if (r.enabled === false) continue;
                key = r.id;
                var hit = inRect(r, p.x, p.y);
                var was = !!inside[p.slot][key];
                if (p.action === "down" && hit) {
                    evts.push({ type: "down", region: r, finger: { slot: f.slot, x: f.x, y: f.y } });
                    inside[p.slot][key] = true;
                } else if (p.action === "move" && hit) {
                    evts.push({ type: "move", region: r, finger: { slot: f.slot, x: f.x, y: f.y } });
                    if (!was) evts.push({ type: "enter", region: r, finger: { slot: f.slot, x: f.x, y: f.y } });
                    inside[p.slot][key] = true;
                } else if (p.action === "up" && (hit || was)) {
                    evts.push({ type: "up", region: r, finger: { slot: f.slot, x: f.x, y: f.y } });
                    delete inside[p.slot][key];
                } else if (hit && !was) {
                    inside[p.slot][key] = true;
                    evts.push({ type: "enter", region: r, finger: { slot: f.slot, x: f.x, y: f.y } });
                } else if (!hit && was) {
                    delete inside[p.slot][key];
                    evts.push({ type: "exit", region: r, finger: { slot: f.slot, x: f.x, y: f.y } });
                }
            }
            if (handlers) {
                for (i = 0; i < evts.length; i++) {
                    var e = evts[i], h = handlers["on" + e.type[0].toUpperCase() + e.type.slice(1)];
                    if (h) h(e.region, e.finger);
                }
            }
            return evts;
        }
    };
    return eng;
}
module.exports = { createEngine: createEngine };
