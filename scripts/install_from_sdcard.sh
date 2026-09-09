#!/system/bin/sh
# Install/start vtouchd from the directory containing this script.
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

for f in vtouchd vtouch-start.sh vtouch-stop.sh; do
    [ -f "$BASE/$f" ] || fail "缺少 $BASE/$f"
done

# Already healthy: do not restart (keeps repeated starts fast).
if [ -f "$RUNDIR/vtouchd.pid" ]; then
    MPID=$(cat "$RUNDIR/vtouchd.pid" 2>/dev/null)
    case "$MPID" in ''|*[!0-9]*) MPID=;; esac
    if [ -n "$MPID" ] && kill -0 "$MPID" 2>/dev/null; then
        say "[OK] VTOUCH_ALREADY_READY=1"
        exit 0
    fi
fi

sh "$BASE/vtouch-stop.sh" >/dev/null 2>&1 || true
mkdir -p "$RUNDIR" 2>/dev/null || fail "无法创建 $RUNDIR"

cp -f "$BASE/vtouchd" "$DST/vtouchd" || fail "复制 vtouchd 失败"
cp -f "$BASE/vtouch-start.sh" "$DST/vtouch-start.sh" || fail "复制 start 脚本失败"
cp -f "$BASE/vtouch-stop.sh" "$DST/vtouch-stop.sh" || fail "复制 stop 脚本失败"
chmod 755 "$DST/vtouchd" "$DST/vtouch-start.sh" "$DST/vtouch-stop.sh" || fail "chmod 失败"

say "[OK] files installed from $BASE"
sh "$DST/vtouch-start.sh"
rc=$?
if [ "$rc" -eq 0 ]; then
    say "[OK] VTOUCH_INSTALL_READY=1"
else
    say "[FAIL] start rc=$rc"
    exit "$rc"
fi
