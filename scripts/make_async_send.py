from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line, *rest=s.split('\n',1)
old='VTouch.prototype._next=function(t){var t=this;if(this.opened&&!this.closed&&!this.busy&&this.queue.length){this.current=this.queue.shift(),this.busy=!0,log("[vtouch] -> "+this.current);try{if(!this.ws.send(this.current))throw new Error("WebSocket send returned false")}catch(t){return log("[vtouch] send failed: "+t),void this.close()}this.replyTimer=setTimeout(function(){log("[vtouch] command timeout: "+t.current),t.close()},this.timeout)}}'
# Locate by boundaries rather than exact minified text.
a=line.index('VTouch.prototype._next=')
b=line.index(',VTouch.prototype._send=',a)
new='VTouch.prototype._next=function(){if(!this.opened||this.closed)return this;while(this.queue.length){this.current=this.queue.shift();log("[vtouch] -> "+this.current);try{if(!this.ws.send(this.current))throw new Error("WebSocket send returned false")}catch(error){log("[vtouch] send failed: "+error),this.close();break}}return this}'
line=line[:a]+new+line[b:]
# Replace response handler's busy/current/timer clearing with simple response logging and no queue gate.
line=line.replace('self._clearReplyTimer(),self.busy=!1,self.current=null;var response=String(text);', 'var response=String(text);')
line=line.replace('self._next()}).on(WebSocket.EVENT_BYTES', '}).on(WebSocket.EVENT_BYTES')
# Ensure send method triggers immediate drain (existing _send does this).
# Remove obsolete timeout clearing from CLOSED/close safely; harmless but no timer is started.
p.write_text(line+'\n'+(rest[0] if rest else ''))
