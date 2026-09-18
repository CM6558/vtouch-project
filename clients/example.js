/**
 * vtouch 调用示例 —— AutoJs6，手机端直接跑（只 require 一个文件）。
 *
 * 前置：什么都不用做。下面这一行 require 会把设备上的核心装好（版本不对就用内嵌的覆盖）、
 *       起好；脚本结束（停止按钮 / 跑到结尾）会自动停掉核心并释放 EVIOCGRAB。
 *       想在退出后留着核心：vt.keepRunning(true)。
 *
 * 坐标：核心用**竖屏逻辑坐标**（固定，不随屏幕旋转变）。device.width/height 在横屏时会是
 *       转过来的尺寸，要自己换算回竖屏；vt.res() 能拿到核心当前的逻辑尺寸与 raw 量程。
 *
 * API 一览：
 *   vt.connect()                      连上核心（已连则复用）
 *   vt.finger() / vt.finger(3)        拿一根手指（省略 slot = 自动挑空闲的 0~9）
 *     .down(x,y) .move(x,y) .up() .tap(x,y[,ms]) .swipe(x1,y1,x2,y2[,ms])
 *   vt.frame([{slot,state,x,y}...])   多指合并进同一帧（state = down/move/up）
 *   vt.onRegion([id,] [事件,] 回调)    区域事件订阅；回调跑在子线程，h={id,ev,slot,x,y,t}
 *   vt.onRegionPress([id,] [选项,] 回调) **一次完整按压 = 一次回调**（推荐）：区域内按下 → 同一手指抬起，
 *                                     回调恰好一次、跑在独立线程（里面可直接 sleep/注入）；返回 { stop }
 *   vt.onTouch([slot,] 回调 [, 事件])  **物理触摸流**：按槽订阅、不按区域过滤，按下→抬起一路跟；
 *                                     **默认只报 down/up**，要移动轨迹写 "down,move,up"；
 *                                     事件**默认带时间戳** h.t（核心采集的时刻），要省流量写 "nots"；
 *                                     h={ev,slot,x,y,t}；vt.follow(slot,cb,事件) 是它的简写
 *   vt.res()                          逻辑尺寸 + raw 量程（字符串）
 *   vt.keepRunning(true) / vt.stop() / vt.alive() / vt.startedByUs()
 */
"use strict";
var vt = require("/sdcard/vtouch.js");

var W = device.width, H = device.height;   // 逻辑坐标（竖屏；横屏时记得换算）

/* ---------- 1. 先看一眼坐标空间 ---------- */
log("核心: " + vt.res());                  // 例：res 1440 3168 raw 0 23040 0 50688

/* ---------- 2. 直接注入 ---------- */
vt.finger().tap(W * 0.5, H * 0.5);                          // 自动挑空闲 slot
vt.finger(3).down(100, 200).move(140, 240).up();            // 显式 slot（0~9）
vt.finger().swipe(W * 0.30, H * 0.80, W * 0.70, H * 0.30, 400);   // 按住时长自己给

vt.frame([                                                  // 多指同一帧按下
    { slot: 0, state: "down", x: W * 0.30, y: H * 0.50 },
    { slot: 1, state: "down", x: W * 0.70, y: H * 0.50 }
]);
sleep(150);                                                 // 想按住就自己 sleep
vt.frame([                                                  // 同一帧抬起
    { slot: 0, state: "up", x: W * 0.30, y: H * 0.50 },
    { slot: 1, state: "up", x: W * 0.70, y: H * 0.50 }
]);

/* ---------- 3. 区域事件：面板里画好的区域，按 id 订阅 ---------- */
/* 事件第二参省略 = down/up/enter/exit（默认不含高频的 move）；要 move 得显式写 "*" 或 "down,move"。
 * 回调跑在**唯一那条分发线程**上、**串行**执行：里面长 sleep / 同步注入会**推迟后面每一条事件**
 * （要慢动作、要注入，请自己 `threads.start(...)` 丢到别的线程去）。
 * move 是「状态」不是消息：同一 (区域, 手指) 只保留**最新一条**（后到的原地覆盖），别指望每条采样都送到。 */
/* ev 语义：down = 按下就命中；enter/exit = 跨越边界；move = 区内移动且位置变了；
 *          up = 抬起时此刻在区域内（从区域外滑进来再抬起也算，配 enter 用）。 */
var REGION_ID = "s3";                                       // ← 面板卡片上的那个 id（你的面板里是 s1/s2/s3）
var downAt = null;                                          // 记一下按下时刻，用来算按压时长
var handle = vt.onRegion(REGION_ID, function (h) {           // 省略事件 = down/up/enter/exit（不含 move）
    /* h.t = 事件发生的墙钟毫秒（跟 Date.now() 同基准）。注意是**手指那一刻**的时间，
     * 所以 up.t - down.t 就是真实按压时长；Date.now() - h.t 则是这段的送达延迟。 */
    log(h.id + " " + h.ev + "  slot=" + h.slot + "  @" + h.x + "," + h.y + "  t=" + h.t);
    if (h.ev === "down" || h.ev === "enter") downAt = h.t;
    if (h.ev === "up" && downAt) log("  → 按压时长 " + (h.t - downAt) + " ms");
    if (h.ev === "down") {                                    // 例：按到区域就点别处
        threads.start(function () { vt.finger().tap(100, h.y); });   // ← 注入别堵在回调里（它会推迟后面每一条事件）
    }
});

/* 不用面板手画也行，同一套协议直接下命令：
 *   var c = vt.connect();
 *   c.cmd("region add c1 0 200 400 1200 1400 1");    // 矩形：<id> 0 x1 y1 x2 y2 启用
 *   c.cmd("region add c2 1 720 2300 260 0 1");       // 圆形：<id> 1 cx cy r 0 启用
 *   vt.listRegions();                               // 看当前表：返回 region 行的数组（按行给）
 * 看表**别用** c.cmd("region list")：表是一区一帧发回来的，而 cmd 一次只回一帧 ——
 * 它只拿到第一行、也拿不到末行 end N；要整表就用 vt.listRegions()（它按行收齐并对账）。
 * 想停监听又不关面板：handle.stop();
 */

/* ---------- 3.5 一次完整按压 = 一次回调（推荐：onRegionPress） ---------- */
/* 需求原型：手指在区域内按下 → **同一根手指抬起**（滑出区域也算这次按压的收尾）→ 做一次动作。
 * onRegionPress 把「只跟本次按压 / 一次性（队列里排队的旧 up 不再触发）/ 先摘 handler 再干慢活 /
 * 慢活丢后台线程」四件事都收进 SDK；回调跑在**独立线程**，所以里面可以直接 sleep、直接注入，
 * 不会堵住事件分发，也不会因为慢动作被重复触发（手搓 onRegion+onTouch 时这几条都得自己记）。
 *   要求"抬起时仍必须在区域内"：vt.onRegionPress(REGION_ID, { insideUp: true }, cb)
 *   需要低层控制（自己管槽位/事件类型/物理流）：仍可用 vt.onRegion(…) + vt.onTouch(…) 组合。 */
var press = vt.onRegionPress(REGION_ID, function (g) {
    log("完整按压：slot" + g.slot + " 从 " + g.down.x + "," + g.down.y
        + " 抬到 " + g.up.x + "," + g.up.y + "，历时 " + g.ms + " ms");
    sleep(2300);                                   // 慢活直接写（回调本来就在独立线程里）
    vt.finger().tap(151, 2251);                    // ← 换成你要点的坐标
    vt.finger().tap(149, 2001);
});
// press.stop();                                   // 不要了就停（还没抬起的追踪也一起收掉）

/* ---------- 4. 生命周期（默认已经替你管好了，这里只是把开关列出来） ---------- */
// vt.keepRunning(true);   // 脚本退出时不要停核心（长驻、别的脚本还要用）
// vt.stop();              // 显式停：SIGTERM → 核心自己收尾（先停面板、再放 EVIOCGRAB）

toastLog("vtouch 示例在跑：面板里按一下区域 " + REGION_ID + " 看日志");
/* 走到这里脚本不会立刻结束（onRegion 起了保活定时器，等手指按）。收尾用 AutoJs6 的停止按钮，
 * 退出钩子会自动停核心 —— 物理触摸立刻回到系统。 */
