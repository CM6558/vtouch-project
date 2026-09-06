from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import hashlib, shutil
root=Path('C:/Users/21102/vtouch-project')
def make_zip(out, files, prefix=''):
    out=root/out
    if out.exists(): out.unlink()
    with ZipFile(out,'w',ZIP_DEFLATED) as z:
        for src, arc in files:
            z.write(root/src, arc)

def tree_files(directory, arc_prefix=''):
    base=root/directory
    return [(p.relative_to(root), str(Path(arc_prefix)/p.relative_to(base)).replace('\\','/')) for p in base.rglob('*') if p.is_file()]

sd=tree_files('sdcard/vtouch-merge','')
ksu=tree_files('ksu-module','')
make_zip('vtouch-merge-sdcard-latest.zip', sd)
make_zip('vtouch-merge-ksu-latest.zip', ksu)
# Bundle keeps both complete packages and the source directory layouts.
bundle=[]
for name in ['vtouch-merge-sdcard-latest.zip','vtouch-merge-ksu-latest.zip']:
    bundle.append((name,name))
bundle += tree_files('sdcard/vtouch-merge','sdcard/vtouch-merge')
bundle += tree_files('ksu-module','ksu-module')
make_zip('vtouch-merge-install-bundle.zip', bundle)
for name in ['vtouch-merge-sdcard-latest.zip','vtouch-merge-ksu-latest.zip','vtouch-merge-install-bundle.zip']:
    p=root/name
    with ZipFile(p) as z: bad=z.testzip(); count=len(z.infolist())
    print(name, p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest(), 'entries='+str(count), 'test='+str(bad))
