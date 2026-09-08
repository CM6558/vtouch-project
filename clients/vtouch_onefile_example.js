"use strict";var VTouch=function(options){options=options||{};this.url=options.url||"ws://127.0.0.1:27183";this.width=options.width||device.width;this.height=options.height||device.height;this.connectTimeout=options.connectTimeout||options.timeout||15000;this.onError=options.onError||null;this.autoStop=options.autoStop!==false;this.ws=null;this.queue=[];this.current=null;this.opened=false;this.connecting=false;this.closed=false;this.keepAlive=null;this.fingers={};this.errStreak=0;this.serviceStarts=0;this.exitHooked=false;this.animThread=null;this.gestureTimers=null;if(options.autoConnect!==false)this.connect();};VTouch.prototype.startService=function(){if(this.serviceStarted)return this;var cmd="B=/data/local/tmp/vtouch-runtime; M=/data/local/tmp/vtouchmerge; W=/data/local/tmp/vtouchws; [ -x $M ] && [ -x $W ] || { echo 'vtouch binaries missing'; exit 11; }; mkdir -p $B; if [ -S $B/merge.sock ] && [ -f $B/merge.pid ] && [ -f $B/websocket.pid ] && kill -0 $(cat $B/merge.pid) 2>/dev/null && kill -0 $(cat $B/websocket.pid) 2>/dev/null; then exit 0; fi; killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; rm -f $B/merge.sock $B/merge.pid $B/websocket.pid; nohup $M -s $B/merge.sock -v 10 -w "+this.width+" -h "+this.height+" >/dev/null 2>&1 </dev/null & echo $! > $B/merge.pid; nohup $W >/dev/null 2>&1 </dev/null & echo $! > $B/websocket.pid";var r=shell(cmd,true);if(r&&r.code!==0)throw new Error("vtouch 服务启动失败: "+(r.error||r.result||""));this.serviceStarted=true;return this;};VTouch.prototype.stopService=function(){try{shell("killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid",true);}catch(error){log("[vtouch] service stop failed: "+error);}this.serviceStarted=false;return this;};VTouch.prototype.connect=function(onReady){var self=this,startedAt=Date.now();this.onReady=onReady||this.onReady||null;if(this.opened||this.connecting)return this;this.closed=false;this.opened=false;this.connecting=true;this.connectAttempts=0;this.startTried=false;if(this.keepAlive){clearInterval(this.keepAlive);this.keepAlive=null;}this.keepAlive=setInterval(function(){if(self.opened&&!self.closed){try{self._send("ping");}catch(e){}}},5000);try{this.startService();}catch(err){log("[vtouch] start service failed: "+err);}function onFail(){if(self.closed||self.opened)return;if(self.connectAttempts%10===0){self.serviceStarts++;try{self.startService();}catch(err){log("[vtouch] start service failed: "+err);}}if(self.connectAttempts>=60||Date.now()-startedAt>=self.connectTimeout)throw new Error("vtouch WebSocket 连接失败");self.retryTimer=setTimeout(attempt,self.connectAttempts<=5?80:200);}function attempt(){if(self.closed||self.opened)return;self.connectAttempts++;log("[vtouch] connecting: "+self.url+" attempt="+self.connectAttempts);self.watchdog=setTimeout(function(){if(!self.opened&&!self.closed){log("[vtouch] connect stalled, retry");try{self.ws&&self.ws.cancel();}catch(e){}onFail();}},600);try{self.ws=new WebSocket(self.url);}catch(err){clearTimeout(self.watchdog);log("[vtouch] create websocket failed: "+err);onFail();return;}self.ws.on(WebSocket.EVENT_OPEN,function(){clearTimeout(self.watchdog);self.connecting=false;self.opened=true;self.closed=false;self.errStreak=0;log("[vtouch] connected: "+(Date.now()-startedAt)+"ms");self._syncFingerState();self._next();if(typeof self.onReady==="function")self.onReady(self);}).on(WebSocket.EVENT_TEXT,function(text){var response=String(text);log("[vtouch] <- "+response);if(response.indexOf("err")===0){self.errStreak++;log("[vtouch] server error: "+response);if(typeof self.onError==="function")self.onError(response);if(self.errStreak>=5){self.errStreak=0;log("[vtouch] too many server errors, resetting virtual touches");try{self.reset();}catch(e){}}}else{self.errStreak=0;}}).on(WebSocket.EVENT_BYTES,function(bytes){log("[vtouch] bytes: "+bytes.utf8());}).on(WebSocket.EVENT_CLOSING,function(code,reason){log("[vtouch] closing: "+code+" "+reason);}).on(WebSocket.EVENT_CLOSED,function(code,reason){clearTimeout(self.watchdog);self.connecting=false;self.opened=false;log("[vtouch] closed: "+code+" "+reason);if(!self.closed)onFail();}).on(WebSocket.EVENT_FAILURE,function(error){clearTimeout(self.watchdog);log("[vtouch] failure: "+error);if(!self.closed)onFail();});}attempt();if(!this.exitHooked){this.exitHooked=true;events.on("exit",function(){try{self.close();}finally{if(self.autoStop){try{self.stopService();}catch(error){log("[vtouch] stop service failed: "+error);}}}});}return this;};VTouch.prototype._syncFingerState=function(){var k;for(k in this.fingers){this.fingers[k].cancel();this.fingers[k].downState=false;}};VTouch.prototype._next=function(){if(!this.opened||this.closed)return this;while(this.queue.length){this.current=this.queue.shift();log("[vtouch] -> "+this.current);try{if(!this.ws.send(this.current))throw new Error("WebSocket send returned false");}catch(error){log("[vtouch] send failed: "+error);this.close();break;}}return this;};VTouch.prototype._send=function(t){if(this.closed)throw new Error("vtouch 已关闭");this.queue.push(t);return this._next();};VTouch.prototype._point=function(t,e){t=Math.round(t);e=Math.round(e);if(t<0||t>=this.width||e<0||e>=this.height)return null;return{x:t,y:e};};VTouch.prototype._slot=function(t){t=Math.round(t);if(t<0||t>9)throw new Error("slot 必须是 0~9");return t;};VTouch.prototype._animate=function(durationMs,tick,done){var self=this;var dur=Math.max(1,Math.round(durationMs==null?300:durationMs));var n=Math.max(2,Math.round(dur/16.667));var interval=dur/n;if(this.animThread&&this.animThread.isAlive&&this.animThread.isAlive()){try{this.animThread.interrupt();}catch(e){}}this.animThread=threads.start(function(){try{for(var i=1;i<=n;i++){sleep(Math.max(1,interval));if(self.closed)return;if(tick)tick(i/n);}if(done)done();}catch(error){log("[vtouch] animation stopped: "+error);}});return this;};VTouch.prototype._clearGestureTimers=function(){var i;if(this.gestureTimers){for(i=0;i<this.gestureTimers.length;i++)clearTimeout(this.gestureTimers[i]);this.gestureTimers=null;}};VTouch.prototype.finger=function(t){var e;if(t===void 0||t===null){for(e=0;e<=9;e++){if(!this.fingers[e]||this.fingers[e].state()==="up")break;}if(e>9)throw new Error("没有可用的模拟手指 slot");t=e;}else{t=this._slot(t);}return this.fingers[t]||(this.fingers[t]=new Finger(this,t)),this.fingers[t];};VTouch.prototype.frame=function(points){var i,o,s,n,valid=[];if(!points||!points.length)throw new Error("frame 不能为空");for(i=0;i<points.length;i++){o=points[i];if(!o||["down","move","up"].indexOf(o.state)<0)throw new Error("state 必须是 down、move 或 up");s=this._slot(o.slot);n=this._point(o.x,o.y);if(!n){log("[vtouch] skip out-of-range frame point "+o.x+","+o.y);continue;}valid.push({slot:s,state:o.state,x:n.x,y:n.y});}if(!valid.length)return this;this._send("begin_frame");for(i=0;i<valid.length;i++){this._send("point "+valid[i].slot+" "+valid[i].state+" "+valid[i].x+" "+valid[i].y);if(this.fingers[valid[i].slot]){if(valid[i].state==="down")this.fingers[valid[i].slot].downState=true;else if(valid[i].state==="up"){this.fingers[valid[i].slot].cancel();this.fingers[valid[i].slot].downState=false;}}}this._send("end_frame");return this;};VTouch.prototype.gesture=function(frames,durationMs){var self=this,i;if(!frames||!frames.length)throw new Error("gesture 不能为空");var dur=Math.max(0,Math.round(durationMs==null?0:durationMs));var per=dur>0?dur/frames.length:0;if(per<=0){for(i=0;i<frames.length;i++)this.frame(frames[i]);return this;}this._clearGestureTimers();this.gestureTimers=[];for(i=0;i<frames.length;i++){(function(frame,delay){self.gestureTimers.push(setTimeout(function(){if(!self.closed)self.frame(frame);},Math.round(delay)));})(frames[i],i*per);}return this;};VTouch.prototype.pinch=function(cx,cy,startGap,endGap,durationMs){var self=this;var r=this._point(cx,cy);if(!r)throw new Error("pinch 中心点超出屏幕: "+cx+","+cy);var maxGap=2*Math.min(r.x,this.width-1-r.x);if(maxGap<2)throw new Error("pinch 中心点不能形成双指: "+r.x+","+r.y);var g0=Math.min(Math.max(1,Math.round(startGap==null?100:startGap)),maxGap);var g1=Math.min(Math.max(1,Math.round(endGap==null?100:endGap)),maxGap);var c=this.finger(0),l=this.finger(1);this.frame([{slot:c.slot,state:"down",x:r.x-g0/2,y:r.y},{slot:l.slot,state:"down",x:r.x+g0/2,y:r.y}]);this._animate(durationMs==null?300:durationMs,function(p){var g=g0+(g1-g0)*p;self.frame([{slot:c.slot,state:"move",x:r.x-g/2,y:r.y},{slot:l.slot,state:"move",x:r.x+g/2,y:r.y}]);},function(){self.frame([{slot:c.slot,state:"up",x:r.x-g1/2,y:r.y},{slot:l.slot,state:"up",x:r.x+g1/2,y:r.y}]);});return this;};VTouch.prototype.reset=function(){var k;for(k in this.fingers){this.fingers[k].cancel();this.fingers[k].downState=false;}this._clearGestureTimers();if(this.animThread&&this.animThread.isAlive&&this.animThread.isAlive()){try{this.animThread.interrupt();}catch(e){}this.animThread=null;}return this._send("reset");};VTouch.prototype.close=function(){var k;for(k in this.fingers){this.fingers[k].cancel();this.fingers[k].downState=false;}this._clearGestureTimers();if(this.animThread&&this.animThread.isAlive&&this.animThread.isAlive()){try{this.animThread.interrupt();}catch(e){}this.animThread=null;}this.closed=true;this.opened=false;this.connecting=false;this.current=null;this.queue=[];if(this.keepAlive){clearInterval(this.keepAlive);this.keepAlive=null;}try{this.ws&&this.ws.close(WebSocket.CODE_CLOSE_NORMAL,"script exit");}catch(e){try{this.ws&&this.ws.cancel();}catch(e2){}}};VTouch.prototype.tap=function(x,y,durationMs){return this.finger().tap(x,y,durationMs);};VTouch.prototype.swipe=function(x1,y1,x2,y2,durationMs){return this.finger().swipe(x1,y1,x2,y2,durationMs);};VTouch.prototype.down=function(x,y){return this.finger().down(x,y);};VTouch.prototype.move=function(x,y){return this.finger().move(x,y);};VTouch.prototype.up=function(){return this.finger().up();};var Finger=function(touch,slot){this.touch=touch;this.slot=slot;this.downState=false;this.timer=null;};Finger.prototype.down=function(x,y){var o=this.touch._point(x,y);if(!o){log("[vtouch] skip out-of-range down "+x+","+y);return this;}this.touch._send("down "+this.slot+" "+o.x+" "+o.y);this.downState=true;return this;};Finger.prototype.move=function(x,y){var o;if(!this.downState){log("[vtouch] skip move: finger "+this.slot+" is not down");return this;}o=this.touch._point(x,y);if(!o){log("[vtouch] skip out-of-range move "+x+","+y);return this;}this.touch._send("move "+this.slot+" "+o.x+" "+o.y);return this;};Finger.prototype.up=function(){if(!this.downState)return this;this.cancel();this.touch._send("up "+this.slot);this.downState=false;return this;};Finger.prototype.tap=function(x,y,durationMs){var self=this;var ms=Math.max(0,Math.round(durationMs==null?60:durationMs));this.down(x,y);if(this.timer)clearTimeout(this.timer);this.timer=setTimeout(function(){self.timer=null;if(self.downState)self.up();},ms);return this;};Finger.prototype.hold=function(durationMs,continuation){var self=this;if(!this.downState){log("[vtouch] hold: finger "+this.slot+" is not down");return this;}if(this.timer)clearTimeout(this.timer);this.timer=setTimeout(function(){self.timer=null;if(self.downState&&continuation)continuation(self);},Math.max(0,Math.round(durationMs)));return this;};Finger.prototype.press=function(x,y,durationMs,continuation){var self=this;this.down(x,y);this.hold(durationMs==null?60:durationMs,function(f){f.up();if(continuation)continuation(f);});return this;};Finger.prototype.swipe=function(x1,y1,x2,y2,durationMs){var self=this;this.down(x1,y1);this.touch._animate(durationMs==null?300:durationMs,function(p){var mx=x1+(x2-x1)*p,my=y1+(y2-y1)*p;if(self.downState)self.move(mx,my);else self.down(mx,my);},function(){if(self.downState)self.up();});return this;};Finger.prototype.frame=function(state,x,y){return this.touch.frame([{slot:this.slot,state:state,x:x,y:y}]);};Finger.prototype.cancel=function(){if(this.timer){clearTimeout(this.timer);this.timer=null;}return this;};Finger.prototype.state=function(){return this.downState?"down":"up";};
/*
 * AutoJs6 SDK v2 使用示例：SDK 已压缩为一行，业务调用保持简洁。
 *
 * 新写法（v2，推荐）：
 *   var vt = new VTouch();        // 自动连接：内部先确保服务就绪再连 WebSocket
 *   vt.tap(540, 1200);            // 一行一个动作，发送队列自动缓冲，无需等待回调
 *   vt.swipe(200, 200, 900, 2000, 1000);
 *
 * 脚本退出时自动 close() + stopService()（events.on("exit") 兜底），
 * 业务代码无需手动收尾（autoStop:false 可关闭自动停服务）。
 *
 * 连接说明（v2 提速）：
 *   connect() 先同步执行 startService()（幂等：服务健康则快速返回，
 *   不健康则拉起），再建立 WebSocket —— 不再经历"先失败一次再重试"，
 *   首次连接即成功；重试间隔前 5 次 80ms、之后 200ms。
 *
 * 旧回调写法（兼容）：
 *   var vt = new VTouch().connect(function (client) { ... });
 *
 * Finger API（f 为 vt.finger() 返回的 Finger 对象）：
 *   vt.finger()         自动分配空闲 slot 0~9
 *   vt.finger(2)        使用指定 slot 2
 *   f.slot              实际 slot 编号
 *   f.down(x, y)        按下并保持
 *   f.move(x, y)        移动（未按下时跳过并告警）
 *   f.up()              抬起并释放 slot
 *   f.tap(x, y, ms)     点击，ms 默认 60
 *   f.swipe(x1,y1,x2,y2,durationMs)  滑动，durationMs 默认 300ms，内部线程按帧插值
 *   f.hold(ms, fn)      按住 ms 后执行回调（回调运行在定时器中，需自行继续 move/up）
 *   f.press(x, y, ms, fn) down 后定时 up，适合自动释放按压
 *   f.frame(state,x,y)  当前手指的 down/move/up 帧操作
 *   f.state()           返回 down 或 up
 *   f.cancel()          取消本地定时器，不会抬起触点
 *
 * VTouch API：
 *   vt.tap(x, y, ms)    便捷单指点击（自动分配手指）
 *   vt.swipe(x1,y1,x2,y2,durationMs)  便捷单指滑动
 *   vt.down(x,y) / vt.move(x,y) / vt.up()  便捷单指（自动分配手指）
 *   vt.frame(points)    多指同帧原子提交（单次 SYN_REPORT）
 *   vt.gesture(frames[, durationMs])  连续提交多帧；第二参可选总时长，
 *                     0 或缺省 = 立即逐帧提交（无内置时序）
 *   vt.pinch(cx,cy,startGap,endGap,durationMs)  双指缩放
 *   vt.reset()          释放全部模拟触点
 *   vt.close()          关闭连接
 *   vt.onError          错误回调，如 vt.onError = function(msg){ log(msg); }
 *
 * 坐标使用 device.width/device.height 的逻辑屏幕坐标，服务端负责转换。
 * sleep(ms) 只阻塞当前 Auto.js 脚本线程；swipe/pinch 已内部线程化，
 * 业务代码如需 sleep 请放进 threads.start。
 */

var vt = new VTouch();      // 自动连接（服务未启动会自动拉起 vtouchmerge + vtouchws）
// vt.onError = function (msg) { log("[vtouch] server error: " + msg); };

/* 样例 1：点击 */
vt.tap(540, 1200, 60);

/* 样例 2：滑动 */
vt.swipe(200, 200, 1500, 2000, 1000);

/* 样例 3：按下、等待、移动、抬起（sleep 放在子线程中执行） */
// threads.start(function () {
//     vt.down(540, 1200);
//     sleep(1000);
//     vt.move(590, 1200);
//     vt.up();
// });

/* 样例 4：显式 slot */
// var f2 = vt.finger(2);
// f2.down(720, 1584).move(760, 1584).up();

/* 样例 5：两个 Finger 同帧移动（原子提交，单次 SYN_REPORT） */
// var f0 = vt.finger(0), f1 = vt.finger(1);
// vt.frame([
//     {slot: f0.slot, state: "down", x: 500, y: 1200},
//     {slot: f1.slot, state: "down", x: 900, y: 1200}
// ]);
// vt.frame([
//     {slot: f0.slot, state: "move", x: 450, y: 1200},
//     {slot: f1.slot, state: "move", x: 950, y: 1200}
// ]);
// vt.frame([
//     {slot: f0.slot, state: "up", x: 450, y: 1200},
//     {slot: f1.slot, state: "up", x: 950, y: 1200}
// ]);

/* 样例 6：双指缩放（durationMs 生效，内部线程插值） */
// vt.pinch(vt.width / 2, vt.height / 2, 200, 1000, 800);

/* 样例 7：手势帧序列（可指定总时长，0/缺省 = 立即逐帧提交） */
// vt.gesture([
//     [
//         {slot: 0, state: "down", x: 500, y: 1200},
//         {slot: 1, state: "down", x: 900, y: 1200}
//     ],
//     [
//         {slot: 0, state: "move", x: 450, y: 1200},
//         {slot: 1, state: "move", x: 950, y: 1200}
//     ],
//     [
//         {slot: 0, state: "up", x: 450, y: 1200},
//         {slot: 1, state: "up", x: 950, y: 1200}
//     ]
// ], 600);

/* 样例 8：hold / press */
// var f = vt.finger();
// f.down(300, 500).hold(1000, function (finger) { finger.move(420, 620).up(); });
// f.press(700, 900, 250, function (finger) { log(finger.state()); });

/* 业务完成后的显式收尾（可选：exit 钩子会自动执行 close + stopService） */
// vt.close();
