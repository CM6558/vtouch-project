from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line=s.splitlines()[0]
needle='VTouch.prototype.connect=function(onReady){'
insert='VTouch.prototype.startService=function(){var r=shell("su -c \\\"sh /sdcard/vtouch-merge/install_from_sdcard.sh\\\"",true),out=String(r&&((r.result!==undefined&&r.result)||r.error)||"");if(!r||r.code!==0||out.indexOf("VTOUCH_READY=1")<0&&out.indexOf("VTOUCH_ALREADY_READY=1")<0)throw new Error("vtouch 服务启动失败: "+out);this.serviceStarted=out.indexOf("VTOUCH_READY=1")>=0;return this};VTouch.prototype.stopService=function(){var r=shell("su -c \\\"sh /data/local/tmp/vtouch-stop.sh\\\"",true);return !!(r&&r.code===0)};'
assert needle in line
line=line.replace(needle,insert+needle,1)
# Ensure the example starts service before connecting.
rest='\n'.join(s.splitlines()[1:])
rest=rest.replace('var vt = new VTouch().connect(function (client) {','var vt = new VTouch().startService().connect(function (client) {',1)
p.write_text(line+'\n'+rest)
