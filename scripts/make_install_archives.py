from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import hashlib
root = Path(__file__).resolve().parent.parent


def make_zip(out, files, prefix=''):
    out = root / out
    if out.exists():
        out.unlink()
    with ZipFile(out, 'w', ZIP_DEFLATED) as z:
        for src, arc in files:
            z.write(root / src, arc)


def tree_files(directory, arc_prefix=''):
    base = root / directory
    return [(p.relative_to(root), str(Path(arc_prefix) / p.relative_to(base)).replace('\\', '/'))
            for p in base.rglob('*') if p.is_file()]


sd = tree_files('sdcard/vtouch-merge', '')
make_zip('vtouch-merge-sdcard-latest.zip', sd)

# Bundle keeps the complete package and the source directory layout.
bundle = [('vtouch-merge-sdcard-latest.zip', 'vtouch-merge-sdcard-latest.zip')]
bundle += tree_files('sdcard/vtouch-merge', 'sdcard/vtouch-merge')
make_zip('vtouch-merge-install-bundle.zip', bundle)

for name in ['vtouch-merge-sdcard-latest.zip', 'vtouch-merge-install-bundle.zip']:
    p = root / name
    with ZipFile(p) as z:
        bad = z.testzip()
        count = len(z.infolist())
    print(name, p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest(), 'entries=' + str(count), 'test=' + str(bad))
