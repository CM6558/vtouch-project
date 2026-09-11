/**
 * 最小「区域触发点击任务」示例 —— vtouch_bundle.js 单文件版
 *
 * 只有两件事是必须的：
 *   1) vt.sub(c)  —— 区域事件（region_ev）只在订阅后推送，不订阅一个字节都收不到；
 *   2) while 常驻  —— 面板就是后端 daemon，脚本一「运行结束」就 events.on("exit") → vt.stop()
 *                    把面板一起收掉（视觉上就是「窗口不弹出」）。
 *
 * 事件两个维度：h.id = 哪个区域（面板里可随时改名），h.slot = 哪根手指（0~9）。
 * rgParseEv 走 daemon 原生事件，id 永远是最新的，脚本不用自己存区域表、不用自己算命中。
 */
var vt = require("/sdcard/vtouch_bundle.js");

var REGION_ID = "s3";        // ← 面板里那个区域的 id（点卡片上的名字可改）

vt.uiStart();                // 面板没起就起（幂等；已在跑则直接复用）
var c = vt.connect();
vt.sub(c);                   // 打开事件通道（同时也会收到 pev 行，不想要就丢掉）
toastLog("监听区域 " + REGION_ID + " …");

events.on("exit", function () { vt.stop(); });   // 释放 EVIOCGRAB + 收掉面板

while (true) {
    var line = c.recv();                 // 非阻塞：没有数据返回 null
    if (!line) { sleep(10); continue; }

    var h = vt.rgParseEv(line);          // {id, ev, slot, x, y}，非 region_ev 行返回 null
    if (!h || h.id !== REGION_ID || h.ev !== "down") continue;

    threads.start(function () {          // 触摸注入不能占着读线程
        vt.finger().tap(h.x, h.y);       // 把「按到这个区域」变成一次点击（也可点任意坐标）
    });
    toastLog(REGION_ID + " 被 slot" + h.slot + " 按下 @ " + h.x + "," + h.y);
}
