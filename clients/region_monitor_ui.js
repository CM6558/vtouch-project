/*
 * region_monitor_ui.js — 监听区域管理 UI (AutoJs6)
 *
 * 用途：可视化管理"触摸监听区域"——预览、创建、拖动、缩放、删除、开关。
 * 基于官方文档：floaty.rawWindow(全屏/穿透/几何方法)、canvas 控件、events.observeTouch、storages。
 *
 * ── 三种模式（可由脚本任意切换）─────────────────────────────
 *   'full'    : 全屏编辑层(可交互) + 编辑工具栏 + 控制胶囊
 *   'preview' : 全屏预览层(触摸穿透，不挡下方 App) + 控制胶囊
 *   'off'     : 全部隐藏，只留右下角 12px 呼吸点(点击可恢复)
 *
 * ── 交互（编辑模式）─────────────────────────────────────────
 *   空白处拖动   → 框选新建区域（松开生效，最小 40px，吸附网格）
 *   点区域       → 选中（高亮 + 四角手柄 + 右上角删除徽标）
 *   拖动区域内部 → 移动（吸附网格）
 *   拖动手柄     → 缩放（吸附网格）
 *   点区域标签片 → 启用/停用（停用区域变灰，不再命中）
 *   点删除徽标   → 删除（可撤销一次）
 *   工具栏       → 新建 / 删除选中 / 撤销 / 网格开关 / 演示轨迹 / 完成 / 隐藏
 *   拖动标题条   → 移动工具栏；拖动胶囊 → 移动胶囊
 *
 * ── 状态可视化 ─────────────────────────────────────────────
 *   实时触摸轨迹(光点+拖尾)、区域命中涟漪、命中计数、启用状态、
 *   左上角状态条("● 监听中 · N 区域 · 命中 M")、可选网格底纹。
 *
 * ── 触摸数据源 ─────────────────────────────────────────────
 *   有 root : events.observeTouch() 真实触摸
 *   无 root : 自动切换"演示轨迹"(参数曲线模拟手指)，便于无设备调试 UI
 *
 * ── 直接运行 ───────────────────────────────────────────────
 *   直接运行本文件：按记忆的模式启动（上次是 full/preview/off 就是什么）。
 *
 * ── 作为模块被其他脚本使用 ──────────────────────────────────
 *   var rm = require('/sdcard/vtouch-merge/clients/region_monitor_ui.js');
 *   rm.start({ mode: 'preview' });   // 只展示预览，不打开管理面板
 *   rm.setMode('off');               // 完全隐藏
 *   rm.addRegion(x, y, w, h);
 *   rm.getRegions();                 // 供监听/过滤逻辑读取
 *   rm.onHit(function (rid, x, y) { ... });  // 命中回调 → 触发业务动作
 *   rm.onChanged(function () { ... });
 *
 *   require 时会按记忆模式自动启动。若想要"被加载但不显示任何东西"：
 *   (1) require 后立刻 rm.setMode('off')；或
 *   (2) require 前设置全局开关（注意不带 var，写成全局）：
 *       regionUiNoAutoStart = true;
 *       var rm = require('region_monitor_ui.js');
 *       rm.start({ mode: 'off' });   // 之后任何时刻再打开
 *
 * ── 跨脚本控制（另一个引擎的脚本）───────────────────────────
 *   storages.create('regionManager').put('cmd', JSON.stringify({
 *       mode: 'preview'   // 'full' | 'preview' | 'off'
 *   }));
 *   本脚本每 600ms 轮询一次并应用。
 *
 * ── 兼容性备注 ─────────────────────────────────────────────
 *   canvas 控件 draw 事件若在你的构建上无效，改用 scriptingJava.md 的
 *   JavaAdapter(android.view.View, {onDraw: ...}) 方案：
 *       var v = new JavaAdapter(android.view.View, { onDraw: drawScene }, overlay.root.getContext());
 *       overlay.root.addView(v);
 *   setOnTouchListener 同理用 JavaAdapter(android.view.View.OnTouchListener, ...)。
 */

var W = device.width, H = device.height;
var MIN = 40;                 // 最小区域边长
var GRID = 48;                // 网格尺寸
var TRAIL_MS = 350;           // 拖尾时长
var RIPPLE_MS = 450;          // 涟漪时长
var PALETTE = [0xFF22D3EE, 0xFFA78BFA, 0xFFFB7185, 0xFFFBBF24, 0xFF34D399, 0xFF60A5FA, 0xFFF472B6, 0xFF38BDF8];

var Paint = android.graphics.Paint;
var Typeface = android.graphics.Typeface;
var MotionEvent = android.view.MotionEvent;
var STYLE = Paint.STYLE;

/* ================= 配置持久化 ================= */

var store = storages.create('regionManager');

function defaultCfg() {
    return {
        mode: 'preview',          // full | preview | off
        seq: 0,                   // 区域 id 计数
        grid: GRID,
        snap: true,
        pillX: Math.round((W - 160) / 2),
        pillY: 18,
        tbX: W - 176,
        tbY: 18,
        regions: []
    };
}

function loadCfg() {
    var c = store.get('cfg');
    if (c) return c;
    var d = defaultCfg();
    store.put('cfg', d);
    return d;
}

var cfg = loadCfg();
var saveTimer = null;

function saveSoon() {
    if (saveTimer) clearTimeout(saveTimer);
    saveTimer = setTimeout(function () { store.put('cfg', cfg); }, 300);
}

function saveNow() {
    if (saveTimer) clearTimeout(saveTimer);
    store.put('cfg', cfg);
}

/* ================= 状态 ================= */

var mode = cfg.mode;          // 当前模式
var lastNonOff = 'preview';   // off 恢复时回到的模式
var snapOn = cfg.snap;
var grid = cfg.grid;
var demoOn = false;           // 演示轨迹开关
var realFeedOk = false;       // 真实触摸是否可用
var selectedId = null;
var undoDeleted = null;       // 撤销缓冲

var trail = [];               // 拖尾点 {x,y,t}
var ripples = [];             // 涟漪 {x,y,t,r0}
var lastFeed = null;          // 上一个触摸点(兼容无 action 字段)
var lastTrailT = 0;
var totalHits = 0;
var lastW = W, lastH = H;

var changedCbs = [];
var hitCbs = [];

var overlay = null, board = null, pill = null, toolbar = null, dot = null;
var tickTimer = null, cmdTimer = null, keepAlive = null;
var started = false;

/* ================= 区域模型 ================= */

function newRegion(x, y, w, h, name) {
    cfg.seq += 1;
    var id = 'r' + cfg.seq;
    return {
        id: id,
        name: name || ('区域 ' + cfg.seq),
        color: PALETTE[cfg.seq % PALETTE.length],
        x: Math.round(x), y: Math.round(y),
        w: Math.round(w), h: Math.round(h),
        enabled: true,
        hits: 0,
        flash: 0
    };
}

function clampRegion(r) {
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.w > W) r.w = W - r.x;
    if (r.y + r.h > H) r.h = H - r.y;
    if (r.w < MIN) r.w = MIN;
    if (r.h < MIN) r.h = MIN;
    if (r.x > W - MIN) r.x = W - MIN;
    if (r.y > H - MIN) r.y = H - MIN;
    return r;
}

function snapV(v) { return snapOn ? Math.round(v / grid) * grid : Math.round(v); }

function regionAt(x, y) {
    for (var i = cfg.regions.length - 1; i >= 0; i--) {
        var r = cfg.regions[i];
        if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) return r;
    }
    return null;
}

function selected() {
    for (var i = 0; i < cfg.regions.length; i++) {
        if (cfg.regions[i].id === selectedId) return cfg.regions[i];
    }
    return null;
}

function selectId(id) { selectedId = id; saveSoon(); }
function deselect() { selectedId = null; }

/* ================= 公开操作 ================= */

function addRegion(x, y, w, h, name) {
    var r = clampRegion(newRegion(x, y, w, h, name));
    cfg.regions.push(r);
    saveSoon();
    fireChanged();
    return r;
}

function removeRegion(id, allowUndo) {
    var r = null;
    for (var i = 0; i < cfg.regions.length; i++) {
        if (cfg.regions[i].id === id) { r = cfg.regions[i]; cfg.regions.splice(i, 1); break; }
    }
    if (!r) return;
    if (allowUndo) undoDeleted = r;
    if (selectedId === id) deselect();
    saveSoon();
    fireChanged();
}

function toggleRegion(id) {
    for (var i = 0; i < cfg.regions.length; i++) {
        if (cfg.regions[i].id === id) {
            cfg.regions[i].enabled = !cfg.regions[i].enabled;
            device.vibrate(12);
            saveSoon();
            fireChanged();
            return;
        }
    }
}

function clearRegions() {
    cfg.regions = [];
    selectedId = null;
    undoDeleted = null;
    saveSoon();
    fireChanged();
}

function getRegions() {
    var out = [];
    for (var i = 0; i < cfg.regions.length; i++) {
        var r = cfg.regions[i];
        out.push({ id: r.id, name: r.name, color: r.color, x: r.x, y: r.y, w: r.w, h: r.h, enabled: r.enabled, hits: r.hits });
    }
    return out;
}

function fireChanged() {
    for (var i = 0; i < changedCbs.length; i++) {
        try { changedCbs[i](); } catch (e) { log(e); }
    }
}

function fireHit(rid, x, y) {
    for (var i = 0; i < hitCbs.length; i++) {
        try { hitCbs[i](rid, x, y); } catch (e) { log(e); }
    }
}

/* ================= 模式管理 =================
 * 悬浮窗一旦 close() 不能再显示（文档），所以 off 模式把窗口移到屏幕外，
 * 而不是关闭；工具栏每次可重建，才用 close+重建。
 */

function setMode(m) {
    if (m !== 'full' && m !== 'preview' && m !== 'off') m = 'preview';
    if (m !== 'off') lastNonOff = m;
    mode = m;
    cfg.mode = m;
    saveSoon();
    ensureWindows();
    if (m === 'off') {
        if (toolbar) { toolbar.close(); toolbar = null; }
        overlay.setTouchable(false);
        overlay.setPosition(-2000, -2000);
        pill.setPosition(-2000, -2000);
        dot.setPosition(W - 34, H - 34);
    } else if (m === 'preview') {
        if (toolbar) { toolbar.close(); toolbar = null; }
        overlay.setTouchable(false);
        overlay.setPosition(0, 0);
        pill.setPosition(cfg.pillX, cfg.pillY);
        dot.setPosition(-1000, -1000);
    } else {
        overlay.setTouchable(true);
        overlay.setPosition(0, 0);
        pill.setPosition(cfg.pillX, cfg.pillY);
        dot.setPosition(-1000, -1000);
        openToolbar();
    }
    fireChanged();
}

function getMode() { return mode; }

/* ================= 悬浮窗构建 ================= */

function ensureWindows() {
    if (overlay) return;
    /* 1. 全屏预览/编辑层（最底） */
    overlay = floaty.rawWindow(
        <frame id="root" w="*" h="*">
            <canvas id="board" w="*" h="*" bg="#00000000"/>
        </frame>
    );
    overlay.setSize(-1, -1);                 // 文档: rawWindow 支持完全全屏
    overlay.setPosition(-2000, -2000);
    board = overlay.board;
    board.on('draw', function (canvas) { drawScene(canvas); });
    board.setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, {
        onTouch: function (v, ev) {
            if (mode !== 'full') return true;
            var a = ev.getAction();
            if (a === MotionEvent.ACTION_DOWN) { gDown(ev.getRawX(), ev.getRawY()); return true; }
            if (a === MotionEvent.ACTION_MOVE) { gMove(ev.getRawX(), ev.getRawY()); return true; }
            if (a === MotionEvent.ACTION_UP)   { gUp(ev.getRawX(), ev.getRawY()); return true; }
            if (a === MotionEvent.ACTION_CANCEL) { gs.kind = 'none'; return true; }
            return true;
        }
    }));

    /* 2. 控制胶囊（中上层）：👁 预览开关  ✏️ 编辑开关  ✕ 隐藏 */
    pill = floaty.rawWindow(
        <card id="pillCard" cardCornerRadius="24" cardBackgroundColor="#F2191929" cardElevation="14">
            <horizontal gravity="center_vertical">
                <text id="pillPreview" text="👁" textSize="20sp" textColor="#FFE2E8F0" gravity="center" w="48" h="44"/>
                <text id="pillEdit" text="✏️" textSize="18sp" textColor="#FFE2E8F0" gravity="center" w="48" h="44"/>
                <text id="pillOff" text="✕" textSize="18sp" textColor="#FF94A3B8" gravity="center" w="48" h="44"/>
            </horizontal>
        </card>
    );
    pill.setPosition(cfg.pillX, cfg.pillY);
    pill.pillPreview.click(function () { setMode(mode === 'full' ? 'preview' : 'off'); });
    pill.pillEdit.click(function () { setMode(mode === 'full' ? 'preview' : 'full'); });
    pill.pillOff.click(function () { setMode('off'); });
    dragWindow(pill, 'pillCard', function (x, y) { cfg.pillX = x; cfg.pillY = y; saveSoon(); });

    /* 3. 右下角恢复点（off 模式的逃生门，最顶） */
    dot = floaty.rawWindow(
        <card id="dotCard" cardCornerRadius="6" cardBackgroundColor="#7722D3EE" w="12" h="12"/>
    );
    dot.setPosition(-1000, -1000);
    dot.dotCard.click(function () { setMode(lastNonOff); });
}

function openToolbar() {
    if (toolbar) return;
    toolbar = floaty.rawWindow(
        <card cardCornerRadius="16" cardBackgroundColor="#F2191929" cardElevation="16" padding="10" w="168">
            <vertical>
                <card id="tbHeader" cardCornerRadius="10" cardBackgroundColor="#3322D3EE" cardElevation="0" padding="8" gravity="center">
                    <text text="✦ 区域编辑器 · 拖动此条" textColor="#FFA5F3FC" textSize="11sp" gravity="center"/>
                </card>
                <vertical marginTop="8">
                    <card id="tbAdd" cardCornerRadius="10" cardBackgroundColor="#FF2A2A3C" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text text="➕ 新建区域" textColor="#FFE2E8F0" textSize="13sp"/>
                    </card>
                    <card id="tbDel" cardCornerRadius="10" cardBackgroundColor="#FF2A2A3C" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text text="🗑 删除选中" textColor="#FFE2E8F0" textSize="13sp"/>
                    </card>
                    <card id="tbUndo" cardCornerRadius="10" cardBackgroundColor="#FF2A2A3C" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text text="↩ 撤销删除" textColor="#FFE2E8F0" textSize="13sp"/>
                    </card>
                    <card id="tbGrid" cardCornerRadius="10" cardBackgroundColor="#FF2A2A3C" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text id="tbGridTxt" text="▦ 网格吸附：开" textColor="#FFE2E8F0" textSize="13sp"/>
                    </card>
                    <card id="tbDemo" cardCornerRadius="10" cardBackgroundColor="#FF2A2A3C" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text id="tbDemoTxt" text="🎯 演示轨迹：关" textColor="#FFE2E8F0" textSize="13sp"/>
                    </card>
                    <card id="tbDone" cardCornerRadius="10" cardBackgroundColor="#FF1E293B" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text text="⏻ 完成编辑" textColor="#FF7DD3FC" textSize="13sp"/>
                    </card>
                    <card id="tbHide" cardCornerRadius="10" cardBackgroundColor="#FF3B1D1D" cardElevation="0" padding="10" marginTop="6" gravity="center">
                        <text text="✕ 隐藏全部" textColor="#FFFCA5A5" textSize="13sp"/>
                    </card>
                </vertical>
            </vertical>
        </card>
    );
    toolbar.setPosition(cfg.tbX, cfg.tbY);

    toolbar.tbAdd.click(function () {
        var w = Math.round(W * 0.7), h = Math.round(H * 0.7);
        var r = addRegion((W - w) / 2, (H - h) / 2, w, h);
        selectId(r.id);
        device.vibrate(30);
        toast('已新建 ' + r.name);
    });
    toolbar.tbDel.click(function () {
        var s = selected();
        if (!s) { toast('先点选一个区域'); return; }
        removeRegion(s.id, true);
        device.vibrate(30);
        toast('已删除（可撤销）');
    });
    toolbar.tbUndo.click(function () {
        if (!undoDeleted) { toast('没有可撤销的删除'); return; }
        var r = undoDeleted;
        cfg.regions.push(r);
        undoDeleted = null;
        selectId(r.id);
        saveSoon(); fireChanged();
        toast('已恢复 ' + r.name);
    });
    toolbar.tbGrid.click(function () {
        snapOn = !snapOn;
        toolbar.tbGridTxt.setText(snapOn ? '▦ 网格吸附：开' : '▦ 网格吸附：关');
        toolbar.tbGrid.setAlpha(snapOn ? 1 : 0.55);
        cfg.snap = snapOn;
        saveSoon();
    });
    toolbar.tbDemo.click(function () {
        demoOn = !demoOn;
        toolbar.tbDemoTxt.setText(demoOn ? '🎯 演示轨迹：开' : '🎯 演示轨迹：关');
        toolbar.tbDemo.setAlpha(demoOn ? 1 : 0.55);
    });
    toolbar.tbDone.click(function () { setMode('preview'); });
    toolbar.tbHide.click(function () { setMode('off'); });
    dragWindow(toolbar, 'tbHeader', function (x, y) { cfg.tbX = x; cfg.tbY = y; saveSoon(); });

    toolbar.tbGrid.setAlpha(snapOn ? 1 : 0.55);
    toolbar.tbDemoTxt.setText(demoOn ? '🎯 演示轨迹：开' : '🎯 演示轨迹：关');
    toolbar.tbDemo.setAlpha(demoOn ? 1 : 0.55);
}

/* 拖动悬浮窗：按在指定 id 控件上拖动整个窗口 */
var dragD = null;

function dragWindow(win, id, onMove) {
    win[id].setOnTouchListener(new JavaAdapter(android.view.View.OnTouchListener, {
        onTouch: function (v, ev) {
            var a = ev.getAction();
            if (a === MotionEvent.ACTION_DOWN) {
                dragD = { dx: ev.getRawX() - win.getX(), dy: ev.getRawY() - win.getY() };
                return true;
            }
            if (a === MotionEvent.ACTION_MOVE && dragD) {
                var nx = Math.round(ev.getRawX() - dragD.dx);
                var ny = Math.round(ev.getRawY() - dragD.dy);
                nx = Math.max(0, Math.min(nx, W - 20));
                ny = Math.max(0, Math.min(ny, H - 40));
                win.setPosition(nx, ny);
                if (onMove) onMove(nx, ny);
                return true;
            }
            if (a === MotionEvent.ACTION_UP || a === MotionEvent.ACTION_CANCEL) {
                dragD = null;
                return true;
            }
            return true;
        }
    }));
}

/* ================= 编辑手势状态机 ================= */

var gs = { kind: 'none', id: null, handle: null, sx: 0, sy: 0, ox: 0, oy: 0 };
var HANDLE = 22;   // 手柄命中半径
var BADGE = 18;    // 删除徽标命中半径（徽标中心在角外 16px）

function hitHandle(r, x, y) {
    var hs = [ [r.x, r.y, 'tl'], [r.x + r.w, r.y, 'tr'], [r.x, r.y + r.h, 'bl'], [r.x + r.w, r.y + r.h, 'br'] ];
    for (var i = 0; i < hs.length; i++) {
        if (Math.abs(x - hs[i][0]) <= HANDLE && Math.abs(y - hs[i][1]) <= HANDLE) return hs[i][2];
    }
    return null;
}

function hitBadge(r, x, y) {
    var bx = r.x + r.w + 16, by = r.y - 16;
    return Math.abs(x - bx) <= BADGE && Math.abs(y - by) <= BADGE;
}

function chipRect(r) {
    fpText.setTextSize(20);
    var tw = fpText.measureText(r.name + ' ×' + r.hits);
    var cw = tw + 26, ch = 30;
    var cx = r.x + 6, cy = r.y + 6;
    if (cx + cw > W - 4) cx = Math.max(4, W - cw - 4);
    if (cy + ch > H - 4) cy = Math.max(4, H - ch - 4);
    return { x: cx, y: cy, w: cw, h: ch };
}

function hitChip(r, x, y) {
    var c = chipRect(r);
    return x >= c.x && x <= c.x + c.w && y >= c.y && y <= c.y + c.h;
}

function gDown(x, y) {
    gs.sx = x; gs.sy = y;
    var s = selected();
    if (s) {
        if (hitBadge(s, x, y)) { removeRegion(s.id, true); device.vibrate(30); return; }
        var h = hitHandle(s, x, y);
        if (h) {
            gs.kind = 'resize'; gs.id = s.id; gs.handle = h;
            gs.ox = s.x; gs.oy = s.y; gs.ow = s.w; gs.oh = s.h;
            return;
        }
        if (hitChip(s, x, y)) { toggleRegion(s.id); return; }
        if (x >= s.x && x <= s.x + s.w && y >= s.y && y <= s.y + s.h) {
            gs.kind = 'move'; gs.id = s.id;
            gs.ox = x - s.x; gs.oy = y - s.y;
            return;
        }
    }
    var r = regionAt(x, y);
    if (r) {
        selectId(r.id);
        gs.kind = 'move'; gs.id = r.id;
        gs.ox = x - r.x; gs.oy = y - r.y;
        return;
    }
    deselect();
    gs.kind = 'create';
    gs.ox = x; gs.oy = y;
}

function gMove(x, y) {
    if (gs.kind === 'create') {
        gs.sx = x; gs.sy = y;        // 橡皮筋跟随
        return;
    }
    var r = findById(gs.id);
    if (!r) { gs.kind = 'none'; return; }
    if (gs.kind === 'move') {
        r.x = snapV(x - gs.ox);
        r.y = snapV(y - gs.oy);
        clampRegion(r);
        saveSoon();
    } else if (gs.kind === 'resize') {
        var x2 = snapV(x), y2 = snapV(y);
        if (gs.handle.indexOf('r') >= 0) { r.w = x2 - r.x; } else { r.w = r.x + r.w - x2; r.x = x2; }
        if (gs.handle.indexOf('b') >= 0) { r.h = y2 - r.y; } else { r.h = r.y + r.h - y2; r.y = y2; }
        if (r.w < MIN) { r.w = MIN; if (gs.handle.indexOf('r') < 0) r.x = r.x + r.w - MIN; }
        if (r.h < MIN) { r.h = MIN; if (gs.handle.indexOf('b') < 0) r.y = r.y + r.h - MIN; }
        clampRegion(r);
        saveSoon();
    }
}

function gUp(x, y) {
    if (gs.kind === 'create') {
        var x1 = Math.min(gs.ox, x), y1 = Math.min(gs.oy, y);
        var x2 = Math.max(gs.ox, x), y2 = Math.max(gs.oy, y);
        if (x2 - x1 >= MIN && y2 - y1 >= MIN) {
            var r = addRegion(x1, y1, x2 - x1, y2 - y1);
            selectId(r.id);
            device.vibrate(30);
            toast('已创建 ' + r.name + ' (' + r.w + '×' + r.h + ')');
        }
    }
    gs.kind = 'none';
}

function findById(id) {
    for (var i = 0; i < cfg.regions.length; i++) {
        if (cfg.regions[i].id === id) return cfg.regions[i];
    }
    return null;
}

/* ================= 触摸数据源 ================= */

function startFeed() {
    if (autojs.isRootAvailable()) {
        try {
            events.observeTouch();
            events.setTouchEventTimeout(16);
            events.onTouch(function (p) {
                var a = (typeof p.action === 'undefined') ? (lastFeed ? 2 : 0) : p.action;
                lastFeed = p;
                feedPoint(p.x, p.y, a);
            });
            realFeedOk = true;
            return;
        } catch (e) {
            log('observeTouch 失败: ' + e);
        }
    }
    demoOn = true;
    toast('无 root，已开启演示轨迹');
}

var demoPhase = 0, demoT0 = 0, demoLastX = 0, demoLastY = 0;

function demoPoint() {
    var now = new Date().getTime();
    var cx = W / 2, cy = H / 2;
    var R = Math.min(W, H) * 0.32;
    var a = now / 1000 * 0.8;
    return {
        x: cx + R * Math.sin(a * 2.3),
        y: cy + R * Math.sin(a * 1.7 + 1.2) * 0.7
    };
}

function demoStep() {
    var now = new Date().getTime();
    if (demoPhase === 0) {
        var p = demoPoint();
        feedPoint(p.x, p.y, 0);    // DOWN
        demoPhase = 1;
        demoT0 = now;
        return;
    }
    if (demoPhase === 1) {
        var p2 = demoPoint();
        feedPoint(p2.x, p2.y, 2);  // MOVE
        if (now - demoT0 > 3000) {
            feedPoint(p2.x, p2.y, 1); // UP
            demoPhase = 2;
            demoT0 = now;
        }
        return;
    }
    if (demoPhase === 2 && now - demoT0 > 900) demoPhase = 0;
}

var curRegion = null;   // 当前手指所在的区域

function feedPoint(x, y, action) {
    if (action === 0) {
        curRegion = regionAt(x, y);
        hitIf(curRegion, x, y);
        pushTrail(x, y);
        return;
    }
    if (action === 2) {
        var r2 = regionAt(x, y);
        if (r2 !== curRegion) { curRegion = r2; hitIf(r2, x, y); }
        pushTrail(x, y);
        return;
    }
    if (action === 1) curRegion = null;
}

function hitIf(r, x, y) {
    if (!r || !r.enabled) return;
    var now = new Date().getTime();
    r.hits += 1;
    r.flash = 1;
    totalHits += 1;
    ripples.push({ x: x, y: y, t: now, r0: Math.min(r.w, r.h) / 2 });
    if (ripples.length > 12) ripples.shift();
    fireHit(r.id, x, y);
}

function pushTrail(x, y) {
    var now = new Date().getTime();
    if (now - lastTrailT < 24 && trail.length > 0) return;
    lastTrailT = now;
    trail.push({ x: x, y: y, t: now });
    if (trail.length > 24) trail.shift();
}

/* ================= 绘制 ================= */

var fpFill = new Paint(); fpFill.setAntiAlias(true);
var fpStroke = new Paint(); fpStroke.setAntiAlias(true); fpStroke.setStyle(STYLE.STROKE);
var fpText = new Paint(); fpText.setAntiAlias(true); fpText.setColor(0xFFE2E8F0); fpText.setTypeface(Typeface.DEFAULT_BOLD);

function mix(c1, c2, t) {
    var r = ((c1 >> 16) & 255) * (1 - t) + ((c2 >> 16) & 255) * t;
    var g = ((c1 >> 8) & 255) * (1 - t) + ((c2 >> 8) & 255) * t;
    var b = (c1 & 255) * (1 - t) + (c2 & 255) * t;
    return (0xFF << 24) | (Math.round(r) << 16) | (Math.round(g) << 8) | Math.round(b);
}

function withAlpha(color, a) {
    return ((a & 255) << 24) | (color & 0xFFFFFF);
}

function drawScene(canvas) {
    if (!canvas) return;
    if (W !== lastW || H !== lastH) {      // 方向/分辨率变化 → 收敛区域
        for (var i = 0; i < cfg.regions.length; i++) clampRegion(cfg.regions[i]);
        lastW = W; lastH = H;
    }
    var now = new Date().getTime();

    if (snapOn && mode === 'full') drawGrid(canvas);
    for (var j = 0; j < cfg.regions.length; j++) drawRegion(canvas, cfg.regions[j], now);
    var s = selected();
    if (s && mode === 'full') drawSelection(canvas, s);
    if (gs.kind === 'create') drawRubber(canvas);
    drawTrail(canvas, now);
    drawRipples(canvas, now);
    drawStatusStrip(canvas);
}

function drawGrid(canvas) {
    for (var gx = 0; gx <= W; gx += grid) {
        fpStroke.setColor((gx % (grid * 4) === 0) ? 0x1AFFFFFF : 0x0DFFFFFF);
        fpStroke.setStrokeWidth(1);
        canvas.drawLine(gx, 0, gx, H, fpStroke);
    }
    for (var gy = 0; gy <= H; gy += grid) {
        fpStroke.setColor((gy % (grid * 4) === 0) ? 0x1AFFFFFF : 0x0DFFFFFF);
        fpStroke.setStrokeWidth(1);
        canvas.drawLine(0, gy, W, gy, fpStroke);
    }
}

function drawRegion(canvas, r, now) {
    if (r.x + r.w < 0 || r.y + r.h < 0 || r.x > W || r.y > H) return;
    var on = r.enabled;
    var base = r.color;

    fpFill.setStyle(STYLE.FILL);
    fpFill.setColor(withAlpha(base, on ? 0x28 : 0x10));
    canvas.drawRoundRect(r.x, r.y, r.x + r.w, r.y + r.h, 12, 12, fpFill);

    fpStroke.setStyle(STYLE.STROKE);
    fpStroke.setStrokeWidth(on ? 2.5 : 1.5);
    var sc = mix(base, 0xFFFFFFFF, Math.max(0, r.flash) * 0.55);
    fpStroke.setColor(on ? sc : 0x66788BA0);
    canvas.drawRoundRect(r.x, r.y, r.x + r.w, r.y + r.h, 12, 12, fpStroke);
    if (r.flash > 0.01) r.flash *= 0.86;

    drawChip(canvas, r);
}

function drawChip(canvas, r) {
    var c = chipRect(r);
    fpFill.setStyle(STYLE.FILL);
    fpFill.setColor(0xD9101018);
    canvas.drawRoundRect(c.x, c.y, c.x + c.w, c.y + c.h, 8, 8, fpFill);
    fpFill.setColor(r.enabled ? 0xFF34D399 : 0xFF64748B);
    canvas.drawCircle(c.x + 12, c.y + c.h / 2, 5, fpFill);
    fpText.setColor(0xFFE2E8F0);
    canvas.drawText(r.name + ' ×' + r.hits, c.x + 22, c.y + c.h / 2 + 7, fpText);
}

function drawSelection(canvas, r) {
    fpStroke.setStyle(STYLE.STROKE);
    fpStroke.setStrokeWidth(6);
    fpStroke.setColor(withAlpha(0xFFFFFFFF, 0x30));
    canvas.drawRoundRect(r.x, r.y, r.x + r.w, r.y + r.h, 12, 12, fpStroke);

    var hs = [ [r.x, r.y], [r.x + r.w, r.y], [r.x, r.y + r.h], [r.x + r.w, r.y + r.h] ];
    for (var i = 0; i < hs.length; i++) {
        fpFill.setStyle(STYLE.FILL);
        fpFill.setColor(0xFF22D3EE);
        canvas.drawCircle(hs[i][0], hs[i][1], 9, fpFill);
        fpStroke.setStrokeWidth(2.5);
        fpStroke.setColor(0xFFFFFFFF);
        canvas.drawCircle(hs[i][0], hs[i][1], 9, fpStroke);
    }
    /* 删除徽标：位于右上角外侧，与手柄错开 */
    var bx = r.x + r.w + 16, by = r.y - 16;
    fpFill.setStyle(STYLE.FILL);
    fpFill.setColor(0xFFEF4444);
    canvas.drawCircle(bx, by, 11, fpFill);
    fpText.setTextSize(16);
    fpText.setColor(0xFFFFFFFF);
    canvas.drawText('✕', bx - 5, by + 6, fpText);
}

function drawRubber(canvas) {
    var x1 = Math.min(gs.ox, gs.sx), y1 = Math.min(gs.oy, gs.sy);
    var x2 = Math.max(gs.ox, gs.sx), y2 = Math.max(gs.oy, gs.sy);
    fpFill.setStyle(STYLE.FILL);
    fpFill.setColor(0x1422D3EE);
    canvas.drawRoundRect(x1, y1, x2, y2, 10, 10, fpFill);
    fpStroke.setStyle(STYLE.STROKE);
    fpStroke.setStrokeWidth(2);
    fpStroke.setColor(0xCC22D3EE);
    canvas.drawRoundRect(x1, y1, x2, y2, 10, 10, fpStroke);
    if (x2 - x1 >= MIN && y2 - y1 >= MIN) {
        fpText.setTextSize(18);
        fpText.setColor(0xFF7DD3FC);
        canvas.drawText('松开创建 ' + Math.round(x2 - x1) + '×' + Math.round(y2 - y1), x1 + 8, y1 + 24, fpText);
    }
}

function drawTrail(canvas, now) {
    for (var i = 0; i < trail.length; i++) {
        var p = trail[i];
        var age = now - p.t;
        if (age > TRAIL_MS) continue;
        var k = 1 - age / TRAIL_MS;
        var rad = 3 + 8 * k;
        fpFill.setStyle(STYLE.FILL);
        fpFill.setColor(withAlpha(0xFF22D3EE, Math.round(70 * k)));
        canvas.drawCircle(p.x, p.y, rad, fpFill);
        fpFill.setColor(withAlpha(0xFF7DD3FC, Math.round(200 * k)));
        canvas.drawCircle(p.x, p.y, Math.max(2, rad * 0.45), fpFill);
    }
}

function drawRipples(canvas, now) {
    for (var i = 0; i < ripples.length; i++) {
        var rp = ripples[i];
        var k = (now - rp.t) / RIPPLE_MS;
        if (k >= 1) continue;
        var p = 1 - Math.pow(1 - k, 3);
        fpStroke.setStyle(STYLE.STROKE);
        fpStroke.setStrokeWidth(3 * (1 - k) + 1);
        fpStroke.setColor(withAlpha(0xFF22D3EE, Math.round(190 * (1 - k))));
        canvas.drawCircle(rp.x, rp.y, 10 + rp.r0 * p, fpStroke);
    }
}

function drawStatusStrip(canvas) {
    var txt = '● 监听中 · ' + cfg.regions.length + ' 区域 · 命中 ' + totalHits;
    if (demoOn) txt += ' · 演示';
    fpText.setTextSize(22);
    var tw = fpText.measureText(txt);
    var pad = 10, cw = tw + pad * 2, ch = 34;
    fpFill.setStyle(STYLE.FILL);
    fpFill.setColor(0xCC101018);
    canvas.drawRoundRect(8, 8, 8 + cw, 8 + ch, 12, 12, fpFill);
    fpFill.setColor(0xFF34D399);
    canvas.drawCircle(8 + pad + 7, 8 + ch / 2, 5, fpFill);
    fpText.setColor(0xFFE2E8F0);
    canvas.drawText(txt, 8 + pad + 18, 8 + ch / 2 + 8, fpText);
}

/* ================= 生命周期 ================= */

function start(opts) {
    if (started) return api();
    started = true;
    opts = opts || {};
    ensureWindows();
    startFeed();
    if (opts.demo === true) demoOn = true;
    if (opts.demo === false) demoOn = false;
    if (opts.mode) setMode(opts.mode);
    else if (cfg.mode === 'full' || cfg.mode === 'preview' || cfg.mode === 'off') setMode(cfg.mode);
    else setMode('preview');

    /* 渲染节拍：30fps（兼 keepalive） */
    tickTimer = setInterval(function () {
        if (mode !== 'off' && board) {
            if (demoOn) demoStep();
            board.invalidate();
        }
    }, 33);

    /* 跨脚本 cmd 轮询 */
    cmdTimer = setInterval(function () {
        var cmd = store.get('cmd');
        if (cmd) {
            store.remove('cmd');
            applyCmd(cmd);
        }
    }, 600);

    keepAlive = setInterval(function () {}, 1000);
    events.on('exit', cleanup);
    fireChanged();
    return api();
}

function applyCmd(cmd) {
    var o = cmd;
    if (typeof cmd === 'string') {
        try { o = JSON.parse(cmd); } catch (e) { log('cmd 解析失败: ' + cmd); return; }
    }
    if (o.mode) setMode(o.mode);
    if (o.regions && o.regions.length > 0) { cfg.regions = o.regions; cfg.seq = cfg.regions.length; saveSoon(); fireChanged(); }
    if (o.clear === true) clearRegions();
    if (o.snap !== undefined) { snapOn = !!o.snap; cfg.snap = snapOn; saveSoon(); }
}

function cleanup() {
    try {
        if (saveTimer) clearTimeout(saveTimer);
        saveNow();
        demoOn = false;
        events.removeAllTouchListeners();
        floaty.closeAll();
    } catch (e) { log(e); }
}

function stop() {
    if (!started) return;
    started = false;
    if (tickTimer) clearInterval(tickTimer);
    if (cmdTimer) clearInterval(cmdTimer);
    if (keepAlive) clearInterval(keepAlive);
    cleanup();
}

/* ================= 模块导出 ================= */

function api() {
    return {
        start: start,
        stop: stop,
        setMode: setMode,
        getMode: getMode,
        addRegion: addRegion,
        removeRegion: removeRegion,
        clearRegions: clearRegions,
        getRegions: getRegions,
        onChanged: function (fn) { changedCbs.push(fn); },
        onHit: function (fn) { hitCbs.push(fn); },
        sendCmd: function (o) { store.put('cmd', JSON.stringify(o)); },
        isRunning: function () { return started; }
    };
}

module.exports = api();

/* 直接运行 = 立即启动（记忆上次模式）；被 require 时可用全局开关 regionUiNoAutoStart 关闭自动启动 */
if (typeof regionUiNoAutoStart === 'undefined' || !regionUiNoAutoStart) {
    start();
}
