from pathlib import Path
import re
p = Path('C:/Users/21102/vtouch-project/clients/vtouch_onefile_example.js')
s = p.read_text()

# 1) startService -> synchronous shell() (Runtime.exec), idempotent, with binary existence check.
start = "VTouch.prototype.startService=function(){if(this.serviceStarted)return this;var cmd=\"B=/data/local/tmp/vtouch-runtime; M=/data/local/tmp/vtouchmerge; W=/data/local/tmp/vtouchws; [ -x $M ] && [ -x $W ] || { echo 'vtouch binaries missing'; exit 11; }; mkdir -p $B; if [ -S $B/merge.sock ] && [ -f $B/merge.pid ] && [ -f $B/websocket.pid ] && kill -0 $(cat $B/merge.pid) 2>/dev/null && kill -0 $(cat $B/websocket.pid) 2>/dev/null; then exit 0; fi; killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; rm -f $B/merge.sock $B/merge.pid $B/websocket.pid; nohup $M -s $B/merge.sock -v 10 -w \"+this.width+\" -h \"+this.height+\" >/dev/null 2>&1 </dev/null & echo $! > $B/merge.pid; nohup $W >/dev/null 2>&1 </dev/null & echo $! > $B/websocket.pid\";var r=shell(cmd,true);if(r&&r.code!==0)throw new Error(\"vtouch 服务启动失败: \"+(r.error||r.result||\"\"));this.serviceStarted=true;return this};"
s, n = re.subn(r'VTouch\.prototype\.startService=.*?(?=\nVTouch\.prototype\.stopService=)', start + '\n', s, count=1, flags=re.S)
assert n == 1, f'startService sub count={n}'

# 2) stopService -> synchronous shell()
stop = "VTouch.prototype.stopService=function(){try{shell(\"killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid\",true)}catch(error){log(\"[vtouch] service stop failed: \"+error)}this.serviceStarted=false;return this};"
s, n = re.subn(r'VTouch\.prototype\.stopService=.*?(?=\nVTouch\.prototype\.connect=)', stop + '\n', s, count=1, flags=re.S)
assert n == 1, f'stopService sub count={n}'

# 3) connect -> watchdog + ensure-service-on-fail + non-blocking keepalive.
connect = "VTouch.prototype.connect=function(onReady){var self=this,startedAt=Date.now();this.onReady=onReady||null;this.closed=false;this.opened=false;this.connectAttempts=0;this.startTried=false;this.keepAlive=setInterval(function(){},1000);function onFail(){if(self.closed||self.opened)return;if(!self.startTried){self.startTried=true;try{self.startService()}catch(err){log(\"[vtouch] start service failed: \"+err);throw err}}if(self.connectAttempts>=60)throw new Error(\"vtouch WebSocket 连接失败\");self.retryTimer=setTimeout(attempt,120)}function attempt(){if(self.closed||self.opened)return;self.connectAttempts++;log(\"[vtouch] connecting: \"+self.url+\" attempt=\"+self.connectAttempts);self.watchdog=setTimeout(function(){if(!self.opened&&!self.closed){log(\"[vtouch] connect stalled, retry\");try{self.ws&&self.ws.cancel()}catch(e){}onFail()}},600);try{self.ws=new WebSocket(self.url)}catch(err){clearTimeout(self.watchdog);log(\"[vtouch] create websocket failed: \"+err);onFail();return}self.ws.on(WebSocket.EVENT_OPEN,function(){clearTimeout(self.watchdog);self.opened=true;self.closed=false;log(\"[vtouch] connected: \"+(Date.now()-startedAt)+\"ms\");self._next();if(typeof self.onReady===\"function\")self.onReady(self)}).on(WebSocket.EVENT_MESSAGE,function(message){log(\"[vtouch] message: \"+message)}).on(WebSocket.EVENT_TEXT,function(text){var response=String(text);log(\"[vtouch] <- \"+response);if(response.indexOf(\"err\")===0)log(\"[vtouch] server error: \"+response)}).on(WebSocket.EVENT_BYTES,function(bytes){log(\"[vtouch] bytes: \"+bytes.utf8())}).on(WebSocket.EVENT_CLOSING,function(code,reason){log(\"[vtouch] closing: \"+code+\" \"+reason)}).on(WebSocket.EVENT_CLOSED,function(code,reason){clearTimeout(self.watchdog);self.opened=false;self.busy=false;self._clearReplyTimer();log(\"[vtouch] closed: \"+code+\" \"+reason);if(!self.closed)onFail()}).on(WebSocket.EVENT_FAILURE,function(error){clearTimeout(self.watchdog);log(\"[vtouch] failure: \"+error);if(!self.closed)onFail()})}attempt();events.on(\"exit\",function(){try{self.close()}finally{try{self.stopService()}catch(error){log(\"[vtouch] stop service failed: \"+error)}}});return this};"
s, n = re.subn(r'VTouch\.prototype\.connect=.*?(?=\nVTouch\.prototype\._clearReplyTimer=)', connect + '\n', s, count=1, flags=re.S)
assert n == 1, f'connect sub count={n}'

# 4) Example: connect() auto-starts service on failure; drop explicit startService().
s = s.replace("var vt = new VTouch().startService().connect(function (client) {",
              "var vt = new VTouch().connect(function (client) {")

# 5) Doc comment update.
s = s.replace("*   var vt = new VTouch().startService().connect(function (client) { ... });\n *   回调只在 WebSocket 连接成功后执行。",
              "*   var vt = new VTouch().connect(function (client) { ... });\n *   连接失败会自动检查并启动服务后重试；回调只在连接成功后执行。")

# 6) Remove the blocking main-thread wait loops that stall the event loop.
s = re.sub(r'\n+// AutoJs6 结束主线程后终止脚本.*$', '\n', s, flags=re.S)

p.write_text(s)
print('rewritten')