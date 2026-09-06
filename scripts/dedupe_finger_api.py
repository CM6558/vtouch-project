from pathlib import Path
p=Path('clients/vtouch_onefile_example.js')
s=p.read_text()
block='Finger.prototype.tap=function(x,y){this.down(x,y);this.up();return this},Finger.prototype.swipe=function(x1,y1,x2,y2,steps){var a=this.touch._point(x1,y1),b=this.touch._point(x2,y2),n=Math.max(1,Math.round(steps||30));this.down(a.x,a.y);for(var i=1;i<=n;i++)this.move(a.x+(b.x-a.x)*i/n,a.y+(b.y-a.y)*i/n);return this.up()},'
pos=s.find(block)
if pos<0: raise SystemExit('block missing')
second=s.find(block,pos+1)
if second>=0: s=s[:second]+s[second+len(block):]
p.write_text(s)
