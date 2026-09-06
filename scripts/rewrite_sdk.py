from pathlib import Path
import re
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line=s.splitlines()[0]
# Replace finger allocator and basic tap mapping.
line=line.replace('VTouch.prototype.finger=function(t){return t=this._slot(t),this.fingers[t]||(this.fingers[t]=new Finger(this,t)),this.fingers[t]}', 'VTouch.prototype.finger=function(t){var e;if(void 0===t||null===t){for(e=0;e<=9;e++)if(!this.fingers[e]||this.fingers[e].state()==="up")break;if(e>9)throw new Error("没有可用的模拟手指 slot");t=e}else t=this._slot(t);return this.fingers[t]||(this.fingers[t]=new Finger(this,t)),this.fingers[t]}')
line=line.replace('VTouch.prototype.tap=function(t,e,o){return this.finger(0).press(t,e,o)}', 'VTouch.prototype.tap=function(t,e,o){var i=this.finger();return i.down(t,e),i.up()}')
# Remove hold/press methods and replace with lifecycle-only Finger methods.
line=re.sub(r',Finger\.prototype\.hold=function\(t,e\).*?,Finger\.prototype\.press=function\(t,e,o,i\).*?,Finger\.prototype\.frame=', ',Finger.prototype.frame=', line)
line=line.replace('Finger.prototype.cancel=function(){return null!==this.timer&&(clearTimeout(this.timer),this.timer=null),this}', 'Finger.prototype.cancel=function(){this.timer=null;return this}')
# Replace frame method's existing behavior is retained; ensure timer-free state.
# Replace docs/example section after SDK semicolon.
head=line+'\n'
docs=r'''/* ==================== Finger 对象 API / 参数说明 ====================
 *
 * var finger = vt.finger();       自动分配一个空闲 slot（0~9）
 * var finger = vt.finger(2);      显式使用 slot 2
 *
 * 坐标使用逻辑屏幕坐标，服务端负责转换为真实触摸轴。
 * finger.down(x, y)               按下并保持，返回 finger
 * finger.move(x, y)               移动已按下的 finger
 * finger.up()                     抬起 finger，并释放其 slot
 * finger.frame(state, x, y)       提交该 finger 的 down/move/up 帧操作
 * finger.state()                  返回 "down" 或 "up"
 * finger.cancel()                 取消本地计划（不会自动抬起已按下触点）
 *
 * 需要阻塞当前 Auto.js 脚本线程时，直接使用 Auto.js 的 sleep：
 * var f = vt.finger();
 * f.down(720, 1584);
 * vt.flush();                     等待当前队列中的命令收到确认
 * sleep(1000);                    只阻塞脚本线程，触点仍保持按下
 * f.move(760, 1584);
 * f.up();
 *
 * vt.flush() 会同步等待当前已排队命令的服务端响应；底层服务和真实触摸不会被阻塞。
 *
 * vt.frame([{slot, state, x, y}, ...]) 提交同一帧多指操作。
 * vt.gesture([frame, ...])         依次提交多个 frame。
 * vt.swipe(x1,y1,x2,y2,options)    使用自动分配/指定 slot 的单指轨迹。
 * vt.pinch(cx,cy,startGap,endGap)  使用两个 Finger 生成双指轨迹。
 * vt.reset()                       释放所有模拟触点并释放 slot。
 * vt.close()                       清理连接和本地状态。
 */

var vt = new VTouch().connect();

/* 自动分配手指，按下后由脚本决定何时继续和释放。 */
var finger = vt.finger();
finger.down(vt.width / 2, vt.height / 2);
vt.flush();
sleep(1000);
finger.move(vt.width / 2 + 50, vt.height / 2);
finger.up();

/* 其它示例： */
// var f0 = vt.finger(0), f1 = vt.finger(1);
// f0.down(500, 1200); f1.down(900, 1200);
// vt.frame([{slot:f0.slot,state:"move",x:450,y:1200},{slot:f1.slot,state:"move",x:950,y:1200}]);
// f0.up(); f1.up();
// vt.reset();
// vt.close();
'''
p.write_text(head+docs)
# Note: flush is added below only if absent.
PY