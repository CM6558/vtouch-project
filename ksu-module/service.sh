# KernelSU service: launch vtouchmerge and the WebSocket bridge.
MODDIR=${0%/*}
RUNDIR=/data/adb/vtouch-merge
LOG="$RUNDIR/merge.log"
WSLOG="$RUNDIR/websocket.log"
PIDFILE="$RUNDIR/merge.pid"
WSPIDFILE="$RUNDIR/websocket.pid"
MERGE="$MODDIR/system/bin/vtouchmerge"
WS="$MODDIR/system/bin/vtouchws"
SIZE="$(wm size 2>/dev/null | sed -n 's/.*Physical size: //p' | head -n 1)"
WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"

mkdir -p "$RUNDIR" 2>/dev/null || exit 1
chmod 0700 "$RUNDIR" 2>/dev/null || true

for f in "$PIDFILE" "$WSPIDFILE"; do
    if [ -f "$f" ]; then
        OLD=$(cat "$f" 2>/dev/null)
        case "$OLD" in ''|*[!0-9]*) OLD=;; esac
        [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null && continue
        rm -f "$f"
    fi
done

[ -f "$MERGE" ] || { echo "missing: $MERGE" >>"$LOG"; exit 1; }
[ -f "$WS" ] || { echo "missing: $WS" >>"$WSLOG"; exit 1; }
case "$WIDTH" in ''|*[!0-9]*) echo "cannot read wm size" >>"$LOG"; exit 1;; esac
case "$HEIGHT" in ''|*[!0-9]*) echo "cannot read wm size" >>"$LOG"; exit 1;; esac
chmod 0755 "$MERGE" "$WS" 2>/dev/null || true

nohup "$MERGE" -s "$RUNDIR/merge.sock" -v 10 -w "$WIDTH" -h "$HEIGHT" >>"$LOG" 2>&1 </dev/null &
MPID=$!
echo "$MPID" >"$PIDFILE"

# Start bridge after the merger has created its Unix socket.
i=0
while [ "$i" -lt 30 ]; do
    [ -S "$RUNDIR/merge.sock" ] && break
    sleep 1
    i=$((i + 1))
done

if [ -S "$RUNDIR/merge.sock" ]; then
    nohup "$WS" >>"$WSLOG" 2>&1 </dev/null &
    echo $! >"$WSPIDFILE"
    chmod 0600 "$PIDFILE" "$WSPIDFILE" "$LOG" "$WSLOG" 2>/dev/null || true
fi
