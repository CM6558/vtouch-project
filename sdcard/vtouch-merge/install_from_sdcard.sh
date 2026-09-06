#!/system/bin/sh
# Install/start vtouch from the directory containing this script.
# The package directory may be anywhere readable by the root terminal.
set +e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd)
BASE=${VTOUCH_PACKAGE_DIR:-$SCRIPT_DIR}
RUNDIR=/data/local/tmp/vtouch-runtime
DST=/data/local/tmp

say(){ printf '%s\n' "$*"; }
fail(){ say "[FAIL] $*"; exit 1; }
[ "$(id -u)" = 0 ] || fail "请先执行 su"
[ -n "$BASE" ] && [ -d "$BASE" ] || fail "找不到安装目录: $BASE"

for f in vtouchmerge vtouchws vtouch-start.sh vtouch-stop.sh; do
    [ -f "$BASE/$f" ] || fail "缺少 $BASE/$f"
done

# If the complete current service is already healthy, do not restart it.
# This keeps repeated Auto.js startup checks fast. Re-run after replacing files
# to force an update (or call vtouch-stop.sh first).
if [ -f "$RUNDIR/merge.pid" ] && [ -f "$RUNDIR/websocket.pid" ] && [ -S "$RUNDIR/merge.sock" ]; then
    MPID=$(cat "$RUNDIR/merge.pid" 2>/dev/null)
    WPID=$(cat "$RUNDIR/websocket.pid" 2>/dev/null)
    case "$MPID:$WPID" in *[!0-9:]*|'') MPID=; WPID=;; esac
    if [ -n "$MPID" ] && [ -n "$WPID" ] && kill -0 "$MPID" 2>/dev/null && kill -0 "$WPID" 2>/dev/null; then
        say "[OK] VTOUCH_ALREADY_READY=1"
        exit 0
    fi
fi

# Stop old instances using the stop script from this same package directory.
sh "$BASE/vtouch-stop.sh" >/dev/null 2>&1 || true
mkdir -p "$RUNDIR" 2>/dev/null || fail "无法创建 $RUNDIR"

cp -f "$BASE/vtouchmerge" "$DST/vtouchmerge" || fail "复制 vtouchmerge 失败"
cp -f "$BASE/vtouchws" "$DST/vtouchws" || fail "复制 vtouchws 失败"
cp -f "$BASE/vtouch-start.sh" "$DST/vtouch-start.sh" || fail "复制 start 脚本失败"
cp -f "$BASE/vtouch-stop.sh" "$DST/vtouch-stop.sh" || fail "复制 stop 脚本失败"
chmod 755 "$DST/vtouchmerge" "$DST/vtouchws" "$DST/vtouch-start.sh" "$DST/vtouch-stop.sh" || fail "chmod 失败"

say "[OK] files installed from $BASE"
sh "$DST/vtouch-start.sh"
rc=$?
if [ "$rc" -eq 0 ]; then
    say "[OK] VTOUCH_INSTALL_READY=1"
else
    say "[FAIL] start rc=$rc"
    exit "$rc"
fi
