#!/system/bin/sh
RUNDIR=/data/adb/vtouch-merge
if [ -f "$RUNDIR/supervisor.pid" ]; then
    PID=$(cat "$RUNDIR/supervisor.pid" 2>/dev/null)
    case "$PID" in ''|*[!0-9]*) PID=;; esac
    [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
    sleep 1
    [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null || true
fi
rm -rf "$RUNDIR"
