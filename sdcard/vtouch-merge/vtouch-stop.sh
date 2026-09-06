#!/system/bin/sh
# Fully stop vtouchmerge/vtouchws and remove runtime state.
set +e
BASE=${VTOUCH_BASE:-/data/local/tmp/vtouch-runtime}
PID="$BASE/merge.pid"
WSPID="$BASE/websocket.pid"

kill_one(){
    p="$1"
    case "$p" in ''|*[!0-9]*) return;; esac
    kill -TERM "$p" 2>/dev/null || true
}
kill_one "$(cat "$PID" 2>/dev/null)"
kill_one "$(cat "$WSPID" 2>/dev/null)"
sleep 1

# Kill descendants/leftovers by exact executable name, including stale instances
# whose pidfiles disappeared. This is intentionally limited to vtouch names.
killall vtouchws 2>/dev/null || true
killall vtouchmerge 2>/dev/null || true
sleep 1
killall -KILL vtouchws 2>/dev/null || true
killall -KILL vtouchmerge 2>/dev/null || true

rm -f "$BASE/merge.sock" "$BASE/merge.pid" "$BASE/websocket.pid"
# Do not remove logs by default; they are useful for diagnosing failed shutdown.

left=0
ps -A 2>/dev/null | grep -E '(^|[[:space:]])(vtouchmerge|vtouchws)([[:space:]]|$)' | grep -v grep >/dev/null 2>&1 && left=1
if [ "$left" = 0 ]; then
    printf '%s\n' '[OK] VTOUCH_STOPPED=1'
    exit 0
fi
printf '%s\n' '[FAIL] vtouch processes remain'
ps -A 2>/dev/null | grep -E 'vtouchmerge|vtouchws' || true
exit 1
