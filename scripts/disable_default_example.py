#!/usr/bin/env python3
from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
# Make the executable example opt-in: SDK loads, examples are comments only.
old='var vt = new VTouch().connect();\n\n/* 自动分配手指，按下后由脚本决定何时继续和释放。 */\nvar finger = vt.finger();\nfinger.down(vt.width / 2, vt.height / 2);\nsleep(1000);\nfinger.move(vt.width / 2 + 50, vt.height / 2);\nfinger.up();'
new='var vt = new VTouch().connect();\n\n/* 默认不自动触摸；复制下面示例到业务脚本后再执行。 */\n/*\nvar finger = vt.finger();\nfinger.down(vt.width / 2, vt.height / 2);\nsleep(1000);\nfinger.move(vt.width / 2 + 50, vt.height / 2);\nfinger.up();\n*/'
if old not in s:
    raise SystemExit('example block not found')
p.write_text(s.replace(old,new))
PY