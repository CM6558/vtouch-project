from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
needle=' * vt.frame([{slot, state, x, y}, ...]) 提交同一帧多指操作。'
assert needle in s
s=s.replace(needle,' * 连接成功后再执行需要 sleep 的业务：new VTouch().connect(function (client) { ... });\n * 不要在 connect() 后立刻 sleep，否则会阻塞 WebSocket EVENT_OPEN 回调。\n *\n'+needle)
p.write_text(s)
PY