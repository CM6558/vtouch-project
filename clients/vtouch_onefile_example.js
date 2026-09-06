"use strict";var VTouch=function(options){options=options||{};this.url=options.url||"ws://127.0.0.1:27183";this.width=options.width||device.width;this.height=options.height||device.height;this.timeout=options.timeout||15000;this.ws=null;this.queue=[];this.current=null;this.busy=false;this.opened=false;this.closed=false;this.replyTimer=null;this.keepAlive=null;this.fingers={}};VTouch.prototype.startService=function(){if(this.serviceStarted)return this;var cmd="B=/data/local/tmp/vtouch-runtime; M=/data/local/tmp/vtouchmerge; W=/data/local/tmp/vtouchws; [ -x $M ] && [ -x $W ] || { echo 'vtouch binaries missing'; exit 11; }; mkdir -p $B; if [ -S $B/merge.sock ] && [ -f $B/merge.pid ] && [ -f $B/websocket.pid ] && kill -0 $(cat $B/merge.pid) 2>/dev/null && kill -0 $(cat $B/websocket.pid) 2>/dev/null; then exit 0; fi; killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; rm -f $B/merge.sock $B/merge.pid $B/websocket.pid; nohup $M -s $B/merge.sock -v 10 -w "+this.width+" -h "+this.height+" >/dev/null 2>&1 </dev/null & echo $! > $B/merge.pid; nohup $W >/dev/null 2>&1 </dev/null & echo $! > $B/websocket.pid";var r=shell(cmd,true);if(r&&r.code!==0)throw new Error("vtouch 服务启动失败: "+(r.error||r.result||""));this.serviceStarted=true;return this};

VTouch.prototype.stopService=function(){try{shell("killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid",true)}catch(error){log("[vtouch] service stop failed: "+error)}this.serviceStarted=false;return this};

VTouch.prototype.connect=function(onReady){var self=this,startedAt=Date.now();this.onReady=onReady||null;this.closed=false;this.opened=false;this.connectAttempts=0;this.startTried=false;this.keepAlive=setInterval(function(){},1000);function onFail(){if(self.closed||self.opened)return;if(!self.startTried){self.startTried=true;try{self.startService()}catch(err){log("[vtouch] start service failed: "+err);throw err}}if(self.connectAttempts>=60)throw new Error("vtouch WebSocket 连接失败");self.retryTimer=setTimeout(attempt,120)}function attempt(){if(self.closed||self.opened)return;self.connectAttempts++;log("[vtouch] connecting: "+self.url+" attempt="+self.connectAttempts);self.watchdog=setTimeout(function(){if(!self.opened&&!self.closed){log("[vtouch] connect stalled, retry");try{self.ws&&self.ws.cancel()}catch(e){}onFail()}},600);try{self.ws=new WebSocket(self.url)}catch(err){clearTimeout(self.watchdog);log("[vtouch] create websocket failed: "+err);onFail();return}self.ws.on(WebSocket.EVENT_OPEN,function(){clearTimeout(self.watchdog);self.opened=true;self.closed=false;log("[vtouch] connected: "+(Date.now()-startedAt)+"ms");self._next();if(typeof self.onReady==="function")self.onReady(self)}).on(WebSocket.EVENT_MESSAGE,function(message){log("[vtouch] message: "+message)}).on(WebSocket.EVENT_TEXT,function(text){var response=String(text);log("[vtouch] <- "+response);if(response.indexOf("err")===0)log("[vtouch] server error: "+response)}).on(WebSocket.EVENT_BYTES,function(bytes){log("[vtouch] bytes: "+bytes.utf8())}).on(WebSocket.EVENT_CLOSING,function(code,reason){log("[vtouch] closing: "+code+" "+reason)}).on(WebSocket.EVENT_CLOSED,function(code,reason){clearTimeout(self.watchdog);self.opened=false;self.busy=false;self._clearReplyTimer();log("[vtouch] closed: "+code+" "+reason);if(!self.closed)onFail()}).on(WebSocket.EVENT_FAILURE,function(error){clearTimeout(self.watchdog);log("[vtouch] failure: "+error);if(!self.closed)onFail()})}attempt();events.on("exit",function(){try{self.close()}finally{try{self.stopService()}catch(error){log("[vtouch] stop service failed: "+error)}}});return this};

VTouch.prototype._clearReplyTimer=function(){null!==this.replyTimer&&(clearTimeout(this.replyTimer),this.replyTimer=null)},VTouch.prototype._next=function(){if(!this.opened||this.closed)return this;while(this.queue.length){this.current=this.queue.shift();log("[vtouch] -> "+this.current);try{if(!this.ws.send(this.current))throw new Error("WebSocket send returned false")}catch(error){log("[vtouch] send failed: "+error),this.close();break}}return this},VTouch.prototype._send=function(t){if(this.closed)throw new Error("vtouch 已关闭");return this.queue.push(t),this._next(),this},VTouch.prototype._point=function(t,e){if(t=Math.round(t),e=Math.round(e),t<0||t>=this.width||e<0||e>=this.height)return null;return{x:t,y:e}},VTouch.prototype._slot=function(t){if((t=Math.round(t))<0||t>9)throw new Error("slot 必须是 0~9");return t},VTouch.prototype.finger=function(t){var e;if(void 0===t||null===t){for(e=0;e<=9;e++)if(!this.fingers[e]||this.fingers[e].state()==="up")break;if(e>9)throw new Error("没有可用的模拟手指 slot");t=e}else t=this._slot(t);return this.fingers[t]||(this.fingers[t]=new Finger(this,t)),this.fingers[t]},VTouch.prototype.frame=function(t){if(!t||!t.length)throw new Error("frame 不能为空");for(var e=0;e<t.length;e++){var o=t[e],i=this._slot(o.slot),n=this._point(o.x,o.y);if(!n){log("[vtouch] skip out-of-range frame point "+o.x+","+o.y);continue}if(["down","move","up"].indexOf(o.state)<0)throw new Error("state 必须是 down、move 或 up");this._send("begin_frame"),this._send("point "+i+" "+o.state+" "+n.x+" "+n.y),this._send("end_frame")}return this},VTouch.prototype.gesture=function(t){if(!t||!t.length)throw new Error("gesture 不能为空");for(var e=0;e<t.length;e++)this.frame(t[e]);return this},VTouch.prototype.pinch=function(t,e,o,i,n){var r=this._point(t,e),h=Math.max(1,Math.round(o)),s=Math.max(1,Math.round(i)),u=2*Math.min(r.x,this.width-1-r.x);if(u<2)throw new Error("pinch 中心点不能形成双指: "+r.x+","+r.y);h=Math.min(h,u),s=Math.min(s,u);var c=this.finger(0),l=this.finger(1),p=r.x-h/2,f=r.x+h/2;c.down(p,r.y),l.down(f,r.y);for(var a=1;a<=30;a++)c.move(p+(r.x-s/2-p)*a/30,r.y),l.move(f+(r.x+s/2-f)*a/30,r.y);return c.up(),l.up()},VTouch.prototype.reset=function(){for(var t in this.fingers)this.fingers[t].cancel(),this.fingers[t].downState=!1;return this._send("reset")},VTouch.prototype.close=function(){for(var t in this.fingers)this.fingers[t].cancel(),this.fingers[t].downState=!1;this.closed=!0,this.opened=!1,this.busy=!1,this.current=null,this.queue=[],this._clearReplyTimer(),null!==this.keepAlive&&(clearInterval(this.keepAlive),this.keepAlive=null);try{this.ws&&this.ws.close(WebSocket.CODE_CLOSE_NORMAL,"script exit")}catch(t){try{this.ws&&this.ws.cancel()}catch(t){}}};var Finger=function(t,e){this.touch=t,this.slot=e,this.downState=!1,this.timer=null};Finger.prototype.tap=function(x,y,durationMs){var self=this,ms=Math.max(0,Math.round(null==durationMs?60:durationMs));this.down(x,y);setTimeout(function(){if(self.downState)self.up()},ms);return this},Finger.prototype.swipe=function(x1,y1,x2,y2,durationMs){var self=this,ms=Math.max(0,Math.round(null==durationMs?300:durationMs)),n=Math.max(2,Math.round(ms/16.667)),i=0,interval=ms/n;this.down(x1,y1);if(0===ms){this.down(x2,y2),this.downState&&this.up();return this}if(this.timer&&this.timer.interrupt)this.timer.interrupt();this.timer=threads.start(function(){try{while(i<n){sleep(Math.max(1,interval));if(!self.downState){var sx=x1+(x2-x1)*i/n,sy=y1+(y2-y1)*i/n;self.down(sx,sy)}else{var mx=x1+(x2-x1)*i/n,my=y1+(y2-y1)*i/n;self.move(mx,my)}i++}if(self.downState)self.up()}catch(error){log("[vtouch] swipe stopped: "+error)}});return this},Finger.prototype.down=function(t,e){var o=this.touch._point(t,e);return o?(this.touch._send("down "+this.slot+" "+o.x+" "+o.y),this.downState=!0):log("[vtouch] skip out-of-range down "+t+","+e),this},Finger.prototype.move=function(t,e){if(!this.downState){log("[vtouch] skip move: finger "+this.slot+" is not down");return this}var o=this.touch._point(t,e);return o?this.touch._send("move "+this.slot+" "+o.x+" "+o.y):log("[vtouch] skip out-of-range move "+t+","+e),this},Finger.prototype.up=function(){return this.downState?(this.cancel(),this.touch._send("up "+this.slot),this.downState=!1,this):this},Finger.prototype.frame=function(t,e,o){return this.touch.frame([{slot:this.slot,state:t,x:e,y:o}])},Finger.prototype.cancel=function(){this.timer=null;return this},Finger.prototype.state=function(){return this.downState?"down":"up"};
/*
 * AutoJs6 Finger API 示例：取消下面的注释即可测试对应功能。
 *
 * 连接：
 *   var vt = new VTouch().connect(function (client) { ... });
 *   回调只在 WebSocket 连接成功后执行；连接失败会自动检查并启动服务后重试。
 *
 * Finger：
 *   client.finger()       自动分配空闲 slot 0~9
 *   client.finger(2)      使用指定 slot 2
 *   f.slot                实际 slot 编号
 *   f.down(x, y)          按下并保持
 *   f.move(x, y)          移动
 *   f.up()                抬起并释放 slot
 *   f.tap(x, y, ms)       点击，ms 默认 60
 *   f.swipe(x1,y1,x2,y2,durationMs)
 *                         滑动，durationMs 默认 300ms
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
        // var f2 = client.finger();
        f.swipe(200, 200, 1500, 2000, 1000);
        // f2.swipe(200, 2000, 1500, 2000, 1000);

        /* 样例 1：点击 */
        // f.tap(client.width / 2, client.height / 2, 60);

        /* 样例 2：按下、等待、移动、抬起 */
        // f.down(client.width / 2, client.height / 2);
        // sleep(1000);
        // f.move(client.width / 2 + 50, client.height / 2);
        // f.up();

        /* 样例 3：单指滑动 */
        // f.swipe(200, 2000, 900, 2000, 1000);

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
