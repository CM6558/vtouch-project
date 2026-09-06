from pathlib import Path
p=Path('C:/Users/21102/vtouch-project/clients/vtouch_onefile_example.js')
s=p.read_text()
# Ensure the current keepalive is a nonblocking timer, then keep the script's main
# thread alive explicitly; AutoJs6 ends a script when its main body returns.
s=s.replace('this.keepAlive=threads.start(function(){while(!self.closed){sleep(100)}});','this.keepAlive=setInterval(function(){},1000);')
needle='''});'''
# target only final example terminator
idx=s.rfind(needle)
if idx < 0: raise SystemExit('example terminator not found')
end=idx+len(needle)
tail='''

// AutoJs6 结束主线程后会终止脚本；等待 WebSocket 真正打开，
// 连接成功后继续保持主线程存活，避免刚发起连接脚本就结束。
while (!vt.opened && !vt.closed) sleep(20);
if (!vt.opened) throw new Error("vtouch WebSocket 连接失败");
while (vt.opened && !vt.closed) sleep(100);
'''
s=s[:end]+tail+s[end:]
p.write_text(s)
print('main thread readiness wait added')
