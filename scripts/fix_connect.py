from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line=s.splitlines()[0]
start=line.index('VTouch.prototype.connect=')
end=line.index(',VTouch.prototype._clearReplyTimer=')
connect='VTouch.prototype.connect=function(onReady){var self=this,startedAt=Date.now();return this.onReady=onReady||null,log("[vtouch] connecting: "+this.url),this.ws=new WebSocket(this.url),this.ws.on(WebSocket.EVENT_OPEN,function(){self.opened=!0,self.closed=!1,log("[vtouch] connected: "+(Date.now()-startedAt)+"ms"),self._next(),"function"==typeof self.onReady&&self.onReady(self)}).on(WebSocket.EVENT_MESSAGE,function(message){log("[vtouch] message: "+message)}).on(WebSocket.EVENT_TEXT,function(text){self._clearReplyTimer(),self.busy=!1,self.current=null;var response=String(text);if(log("[vtouch] <- "+response),0===response.indexOf("err"))return log("[vtouch] server error: "+response),void self.close();self._next()}).on(WebSocket.EVENT_BYTES,function(bytes){log("[vtouch] bytes: "+bytes.utf8())}).on(WebSocket.EVENT_CLOSING,function(code,reason){log("[vtouch] closing: "+code+" "+reason)}).on(WebSocket.EVENT_CLOSED,function(code,reason){self.opened=!1,self.busy=!1,self._clearReplyTimer(),log("[vtouch] closed: "+code+" "+reason)}).on(WebSocket.EVENT_FAILURE,function(error){log("[vtouch] failure: "+error),self.close()}),this.keepAlive=setInterval(function(){},1e3),events.on("exit",function(){self.close()}),this}'
p.write_text(connect+line[end:]+"\n"+"\n".join(s.splitlines()[1:]))
PY