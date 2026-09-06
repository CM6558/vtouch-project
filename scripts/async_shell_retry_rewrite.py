from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line, *rest=s.split('\n',1)
# Add persistent shell fields to constructor.
line=line.replace('this.fingers={}};', 'this.fingers={};this.shell=null;this.retryTimer=null;this.connectAttempts=0};', 1)
# Replace startService through before stopService.
a=line.index('VTouch.prototype.startService=')
b=line.index('VTouch.prototype.stopService=', a)
start='VTouch.prototype.startService=function(){if(this.serviceStarted)return this;this.shell=new Shell(true);this.shell.exec("BASE=/data/local/tmp/vtouch-runtime;M=/data/local/tmp/vtouchmerge;W=/data/local/tmp/vtouchws;S=$BASE/merge.sock;P=$BASE/merge.pid;Q=$BASE/websocket.pid;mkdir -p $BASE||exit 1;if [ -S $S ]&&[ -f $P ]&&[ -f $Q ];then mp=$(cat $P);wp=$(cat $Q);kill -0 $mp 2>/dev/null&&kill -0 $wp 2>/dev/null&&exit 0;fi;killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f $S $P $Q;z=$(wm size|sed -n \'s/.*Physical size: //p\');w=${z%x*};h=${z#*x};nohup $M -s $S -v 10 -w $w -h $h >/dev/null 2>&1 </dev/null & echo $!>$P;nohup $W >/dev/null 2>&1 </dev/null & echo $!>$Q");this.serviceStarted=true;return this};'
line=line[:a]+start+line[b:]
# Replace stopService through connect.
a=line.index('VTouch.prototype.stopService=')
b=line.index('VTouch.prototype.connect=', a)
stop='VTouch.prototype.stopService=function(){try{if(this.shell){this.shell.exec("killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid");this.shell.exit();this.shell=null}else shell("killall vtouchmerge 2>/dev/null;killall vtouchws 2>/dev/null;rm -f /data/local/tmp/vtouch-runtime/merge.sock /data/local/tmp/vtouch-runtime/merge.pid /data/local/tmp/vtouch-runtime/websocket.pid",false)}catch(error){log("[vtouch] service stop failed: "+error)}};'
line=line[:a]+stop+line[b:]
# Replace connect function through _clearReplyTimer.
a=line.index('VTouch.prototype.connect=')
b=line.index('VTouch.prototype._clearReplyTimer=', a)
connect='VTouch.prototype.connect=function(onReady){var self=this,startedAt=Date.now();this.onReady=onReady||null;this.connectAttempts=0;function attempt(){if(self.closed)return;self.connectAttempts++;log("[vtouch] connecting: "+self.url+" attempt="+self.connectAttempts);self.ws=new WebSocket(self.url);self.ws.on(WebSocket.EVENT_OPEN,function(){self.opened=true;self.closed=false;log("[vtouch] connected: "+(Date.now()-startedAt)+"ms");self._next();if(typeof self.onReady==="function")self.onReady(self)}).on(WebSocket.EVENT_MESSAGE,function(message){log("[vtouch] message: "+message)}).on(WebSocket.EVENT_TEXT,function(text){var response=String(text);if(log("[vtouch] <- "+response),response.indexOf("err")===0){log("[vtouch] server error: "+response);self.close()} }).on(WebSocket.EVENT_BYTES,function(bytes){log("[vtouch] bytes: "+bytes.utf8())}).on(WebSocket.EVENT_CLOSING,function(code,reason){log("[vtouch] closing: "+code+" "+reason)}).on(WebSocket.EVENT_CLOSED,function(code,reason){self.opened=false;self._clearReplyTimer();log("[vtouch] closed: "+code+" "+reason);if(!self.closed){self.retryTimer=setTimeout(attempt,100)}}).on(WebSocket.EVENT_FAILURE,function(error){log("[vtouch] failure: "+error);if(!self.closed){self.retryTimer=setTimeout(attempt,100)}})}attempt();this.keepAlive=setInterval(function(){},1000);events.on("exit",function(){try{self.close()}finally{try{self.stopService()}catch(error){log("[vtouch] stop service failed: "+error)}}});return this};'
line=line[:a]+connect+line[b:]
# Remove automatic business execution? retain current.
p.write_text(line+'\n'+(rest[0] if rest else ''))
