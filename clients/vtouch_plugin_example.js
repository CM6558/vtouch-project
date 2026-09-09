/* ============================================================================
 * VTouch 应用插件版 · 最简可用版 + 完整文档（v3.0.0）
 *
 * ---- 最简代码（复制即用） ----
 * var VTouch = plugins.load('org.vtouch.plugin');
 * var done = false;
 * device.wakeUpIfNeeded();
 * threads.start(function () {
 *     try {
 *         device.keepScreenOn(60 * 1000);
 *         var vt = new VTouch();      // 自动拉起后端 + 自动连接
 *         vt.ready();                 // 等连上（业务线程内）
 *         vt.finger().tap(540, 1200); // 点一下
 *         vt.close();                 // 发 reset + 关连接
 *         device.cancelKeepingAwake();
 *     } catch (e) { toastLog("vtouch 失败: " + e); }
 *     done = true;
 *     exit();
 * });
 * while (!done) sleep(200);
 *
 * ---- 1. 加载方式 ----
 *   plugins.load('org.vtouch.plugin')   应用插件（需安装 vtouch-plugin.apk，
 *                                       任意目录脚本可用，推荐）
 *   plugins.load('vtouch')              项目插件（脚本须在项目目录内）
 *   require("/sdcard/vtouch.js")        直接引用（无需插件，SDK 文件放通可读处）
 *   加载返回值即 VTouch 构造函数；VTouch.VERSION 查看版本，VTouch.Finger 即 Finger 类。
 *
 * ---- 2. 构造与连接 ----
 *   var vt = new VTouch();              自动拉起后端 + 自动连接（二合一）
 *   var vt = new VTouch({               全部可选参数：
 *       url: "ws://127.0.0.1:27183",    后端地址（默认本机）
 *       timeout: 10000,                 连接超时毫秒（默认 10000）
 *       autoStop: true,                 脚本退出时自动停后端（默认 true；
 *                                       设 false 则后端常驻，下次启动 50ms 级）
 *       autoConnect: true               设 false 则只构造不连接，稍后手动 vt.connect()
 *   });
 *   vt.connect(onReady)                 手动连接；onReady(conn) 连接成功回调，可链式调用
 *   vt.ready(timeout)                   阻塞等到连接成功（内部 sleep，必须在业务线程调）
 *   vt.close()                          发 reset + 关连接 + 清队列；业务结束调一次
 *   vt.status()                         后端自检（APK 通道）：pid/端口/READY，一行看完
 *   vt.stopService()                    停后端、释放触摸独占；autoStop 已包办，仅需手动常驻管理时调
 *
 * ---- 3. Finger（单指，核心 API） ----
 *   var f = vt.finger();                自动取空闲 slot（0~9），满了抛错
 *   var f2 = vt.finger(2);              指定 slot 2（多指各用各的 slot）
 *   f.down(x, y)                        按下；坐标为逻辑像素（与 device.width/height 一致）
 *   f.move(x, y)                        移动；未 down 时空操作（不报错）
 *   f.up()                              抬起；未 down 时空操作
 *   f.tap(x, y, ms)                     点击 = down + sleep(ms,默认60) + up
 *   f.swipe(x1, y1, x2, y2, ms)         滑动 = down + 约每 16ms 插值 move + up，ms 默认 300
 *   f.frame(state, x, y)                把单指动作并入原子帧（state: down/move/up）
 *   f.state()                           "down" 或 "up"，断言防卡指
 *   以上除特别注明外都返回 f，可链式：f.down(100,200).move(150,200).up()
 *
 * ---- 4. 多指原子帧 ----
 *   vt.frame([                          一帧内多点同报，App 侧一次收到（ pinch 必备）
 *       { slot: 0, state: "down", x: 500, y: 1200 },
 *       { slot: 1, state: "down", x: 900, y: 1200 }
 *   ]);
 *   同一 slot 在一帧里只能出现一次；state 三选一；越界点自动跳过并打日志。
 *
 * ---- 5. 线程铁律（必读） ----
 *   构造 + tap / swipe / ready 全放业务线程（threads.start），主线程只做
 *   while(!done) sleep 保活。主线程提前结束会带走 WebSocket 事件循环，
 *   表现为 daemon 空闲、10 秒超时。退出用 exit()，清理自动走。
 *   后台运行时系统可能冻结脚本线程（现象：一次跑几十秒、ready 超时但后端明明活着）：
 *   开头加 wakeUpIfNeeded + keepScreenOn，把 AutoJs6 切前台，并去“电池优化”里把
 *   AutoJs6 设为不优化，可基本消除。
 *
 * ---- 6. 常用组合（都在业务线程里写） ----
 *   长按 1 秒：  f.down(x, y); sleep(1000); f.up();
 *   双指缩放：  先 frame 双 down → sleep 中多次 frame 双 move → frame 双 up
 *   三指同按：  vt.frame() 一次塞三个 down 点
 *   中途复位：  vt.reset()（放开全部虚拟手指，物理触摸不受影响）
 *
 * ---- 7. 排错 ----
 *   "vtouch WS 连接超时"  后端没起来：APK 通道跑 vt.status() 看 pid/端口哪项不过
 *   "无空闲 slot"         有手指没 up：检查逻辑，或 vt.reset() 统一放开
 *   日志 "[vtouch] err:"  单条命令被服务端拒绝（越界/状态非法），不影响后续
 *   触摸无反应            先确认物理触摸正常，再看 vtouchd 进程与 27183 端口在不在
 * ============================================================================
 */
"use strict";

var VTouch = plugins.load('org.vtouch.plugin');
var done = false;
var t0 = Date.now();
device.wakeUpIfNeeded();      // 亮屏：灭屏下系统会冻结脚本线程，表现为十几秒无响应
threads.start(function () {
    try {
        device.keepScreenOn(60 * 1000);   // 60 秒内不休眠（脚本结束前取消）
        var vt = new VTouch();
        vt.ready();
        vt.finger().tap(540, 1200);
        vt.close();
        device.cancelKeepingAwake();
    } catch (e) {
        toastLog("vtouch 失败: " + e);
    }
    done = true;
    exit();
});
while (!done && Date.now() - t0 < 30000) sleep(200);
