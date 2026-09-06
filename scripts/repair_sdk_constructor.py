from pathlib import Path
p=Path('sdcard/vtouch-merge/vtouch_onefile_example.js')
s=p.read_text()
constructor='"use strict";var VTouch=function(options){options=options||{};this.url=options.url||"ws://127.0.0.1:27183";this.width=options.width||device.width;this.height=options.height||device.height;this.timeout=options.timeout||15000;this.ws=null;this.queue=[];this.current=null;this.busy=false;this.opened=false;this.closed=false;this.replyTimer=null;this.keepAlive=null;this.fingers={}};'
if s.startswith('VTouch.prototype.connect='):
    s=constructor+s
# fix connect function variable prefix if needed
s=s.replace('VTouch.prototype.connect=function(onReady){var self=','VTouch.prototype.connect=function(onReady){var self=',1)
p.write_text(s)
Path('clients/vtouch_onefile_example.js').write_text(s)
