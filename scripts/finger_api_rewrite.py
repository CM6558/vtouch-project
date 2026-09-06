from pathlib import Path
import re
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
line, *rest=s.split('\n',1)
# Remove VTouch single-finger convenience methods, preserving frame onward.
line=re.sub(r',VTouch\.prototype\.down=.*?,VTouch\.prototype\.frame=', ',VTouch.prototype.frame=', line)
# Remove VTouch swipe method, preserving pinch.
line=re.sub(r',VTouch\.prototype\.swipe=.*?,VTouch\.prototype\.pinch=', ',VTouch.prototype.pinch=', line)
# Add Finger tap/swipe immediately after Finger constructor.
needle='var Finger=function(t,e){this.touch=t,this.slot=e,this.downState=!1,this.timer=null};'
insert=needle+'Finger.prototype.tap=function(x,y){this.down(x,y);this.up();return this},Finger.prototype.swipe=function(x1,y1,x2,y2,steps){var a=this.touch._point(x1,y1),b=this.touch._point(x2,y2),n=Math.max(1,Math.round(steps||30));this.down(a.x,a.y);for(var i=1;i<=n;i++)this.move(a.x+(b.x-a.x)*i/n,a.y+(b.y-a.y)*i/n);return this.up()},'
if needle not in line: raise SystemExit('Finger constructor not found')
line=line.replace(needle,insert,1)
# Update documentation text in remaining readable section.
body=rest[0] if rest else ''
body=body.replace(' * vt.frame([{slot, state, x, y}, ...]) 提交同一帧多指操作。\n * vt.gesture([frame, ...])         依次提交多个 frame。\n * vt.swipe(x1,y1,x2,y2,options)    使用自动分配/指定 slot 的单指轨迹。\n * vt.pinch(cx,cy,startGap,endGap)  使用两个 Finger 生成双指轨迹。', ' * f.tap(x, y)                      使用当前 Finger 点击：down -> up。\n * f.swipe(x1,y1,x2,y2,steps)       使用当前 Finger 滑动；steps 默认 30。\n * vt.frame([{slot, state, x, y}, ...]) 提交同一帧多指操作。\n * vt.gesture([frame, ...])         依次提交多个 frame。\n * vt.pinch(cx,cy,startGap,endGap)  使用两个 Finger 生成双指轨迹。')
body=body.replace('// var f0 = vt.finger(0), f1 = vt.finger(1);', '// var f0 = vt.finger(0), f1 = vt.finger(1);\n// f0.tap(500, 1200);\n// f0.swipe(300, 1000, 900, 1000, 30);')
p.write_text(line+'\n'+body)
