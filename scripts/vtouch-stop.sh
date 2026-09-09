#!/system/bin/sh
# Fully stop vtouchd (plus legacy vtouchmerge/vtouchws) and remove runtime state.
set +e
BASE=${VTOUCH_BASE:-/data/local/tmp/vtouch-runtime}
PID="$BASE/vtouchd.pid"

kill_one(){
    p="$1"
    case "$p" in ''|*[!0-9]*) return;; esac
    kill -TERM "$p" 2>/dev/null || true
}
kill_one "$(cat "$PID" 2>/dev/null)"
sleep 1

killall vtouchd 2>/dev/null || true
killall vtouchmerge 2>/dev/null || true
killall vtouchws 2>/dev/null || true
sleep 1
killall -KILL vtouchd 2>/dev/null || true
killall -KILL vtouchmerge 2>/dev/null || true
killall -KILL vtouchws 2>/dev/null || true

rm -f "$BASE/vtouchd.pid" "$BASE/merge.sock" "$BASE/merge.pid" "$BASE/websocket.pid"

left=0
ps -A 2>/dev/null | grep -E '(^|[[:space:]])(vtouchd|vtouchmerge|vtouchws)([[:space:]]|$)' | grep -v grep >/dev/null 2>&1 && left=1
if [ "$left" = 0 ]; then
    printf '%s\n' '[OK] VTOUCH_STOPPED=1'
    exit 0
fi
printf '%s\n' '[FAIL] vtouch processes remain'
ps -A 2>/dev/null | grep -E 'vtouchd|vtouchmerge|vtouchws' || true
exit 1
