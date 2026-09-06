from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line=s.splitlines()[0]
suffix=r'''/*
 * AutoJs6 Finger API 示例：取消下面的注释即可测试对应功能。
 *
 * 连接：
 *   var vt = new VTouch().connect(function (client) { ... });
 *   回调只在 WebSocket 连接成功后执行。
 *
 * Finger：
 *   client.finger()       自动分配空闲 slot 0~9
 *   client.finger(2)      使用指定 slot 2
 *   f.slot                实际 slot 编号
 *   f.down(x, y)          按下并保持
 *   f.move(x, y)          移动
 *   f.up()                抬起并释放 slot
 *   f.tap(x, y, ms)       点击，ms 默认 60
 *   f.swipe(x1,y1,x2,y2,ms,steps)
 *                         滑动，ms 默认 300，steps 默认 30
 *   f.frame(state,x,y)    当前手指的 down/move/up 帧操作
 *   f.state()             返回 down 或 up
 *   f.cancel()            清理本地状态，不会抬起触点
 *
 * VTouch：
 *   client.frame(points)  多指同帧操作
 *   client.gesture(frames)连续提交多帧
 *   client.pinch(cx,cy,startGap,endGap)
 *   client.reset()        释放全部模拟触点
 *   client.close()        关闭连接
 *
 * 坐标使用 device.width/device.height 的逻辑屏幕坐标，服务端负责转换。
 * sleep(ms) 只阻塞当前 Auto.js 脚本线程；如需 sleep，放进 threads.start。
 */

var vt = new VTouch().connect(function (client) {
    threads.start(function () {
        var f = client.finger();

        /* 样例 1：点击 */
        // f.tap(client.width / 2, client.height / 2, 60);

        /* 样例 2：按下、等待、移动、抬起 */
        // f.down(client.width / 2, client.height / 2);
        // sleep(1000);
        // f.move(client.width / 2 + 50, client.height / 2);
        // f.up();

        /* 样例 3：单指滑动 */
        // f.swipe(200, 2000, 900, 2000, 1000, 40);

        /* 样例 4：显式 slot */
        // var f2 = client.finger(2);
        // f2.down(720, 1584).move(760, 1584).up();

        /* 样例 5：两个 Finger 同帧移动 */
        // var f0 = client.finger(0), f1 = client.finger(1);
        // f0.down(500, 1200); f1.down(900, 1200);
        // client.frame([
        //     {slot:f0.slot, state:"move", x:450, y:1200},
        //     {slot:f1.slot, state:"move", x:950, y:1200}
        // ]);
        // f0.up(); f1.up();

        /* 样例 6：双指缩放 */
        // client.pinch(client.width / 2, client.height / 2, 200, 1000);

        /* 样例 7：手势帧序列 */
        // client.gesture([
        //     [
        //         {slot:0, state:"down", x:500, y:1200},
        //         {slot:1, state:"down", x:900, y:1200}
        //     ],
        //     [
        //         {slot:0, state:"move", x:450, y:1200},
        //         {slot:1, state:"move", x:950, y:1200}
        //     ],
        //     [
        //         {slot:0, state:"up", x:450, y:1200},
        //         {slot:1, state:"up", x:950, y:1200}
        //     ]
        // ]);

        /* 样例 8：异常清理 */
        // client.reset();
        // client.close();
    });
});
'''
p.write_text(line+'\n'+suffix)
