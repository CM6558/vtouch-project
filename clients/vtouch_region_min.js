/**
 * 最小「区域触发任务」示例 —— vtouch_bundle.js 单文件版（仪式全收进库）
 *
 * 现在只剩一件事必须写：onRegion(区域 id, [事件], 回调)。库里自动完成：
 *   uiStart（面板 = 后端）→ connect → sub → 读线程 → 事件过滤 → 回调丢子线程
 *   → 脚本退出收尾（释放 EVIOCGRAB）→ 主线程保活。
 *
 * 事件（第二参）规则：**不指定 = down / up / enter / exit（默认不含 move）**；
 *   指定就只传指定的，可以给 "down" / "up" / "enter" / "exit" / "move"；
 *   要多个写成 "down,move" 或 ["down","move"]；"*" / "any" = 全部（含 move）。
 *   为什么默认不含 move：它是高频事件（手指在区域内每帧一条），灌进业务回调没意义。
 *
 * 回调收到 h：
 *   id   命中区域的名字（面板卡片名，改名后永远是最新的）
 *   ev   down / up / enter / exit / move
 *   slot 哪根手指（0~9）
 *   x,y  逻辑坐标（当前屏幕坐标，直接用）
 *
 * 回调已经跑在子线程，里面可以直接写含 sleep 的动作（长按/拖拽 = down → sleep → move → up）。
 * 不限区域：vt.onRegion(function (h) { … });            // 所有区域 + 全部事件
 *           vt.onRegion(function (h) { … }, "up");     // 所有区域 + 只要抬起
 * 想中途停监听用返回的句柄 handle.stop()（不断面板；面板归脚本退出时的 exit 钩子收）。
 */
var vt = require("/sdcard/vtouch_bundle.js");

var REGION_ID = "s3";        // ← 面板里那个区域的 id（写错/被禁用/面板没画，启动时会立刻 toast 提示）

vt.onRegion(REGION_ID, "down", function (h) {
    toastLog(REGION_ID + " 被 slot" + h.slot + " 按下 @" + h.x + "," + h.y);
    vt.finger().tap(h.x, h.y);       // 把「按到这个区域」变成一次点击（也可以点任意坐标）
});
