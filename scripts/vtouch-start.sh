#!/system/bin/sh
# Fast start: files are installed by install_from_sdcard.sh.
# Run: su -c 'sh /data/local/tmp/vtouch-start.sh'
set +e
BASE=${VTOUCH_BASE:-/data/local/tmp/vtouch-runtime}
MERGE=${VTOUCH_MERGE:-/data/local/tmp/vtouchmerge}
WS=${VTOUCH_WS:-/data/local/tmp/vtouchws}
SOCK="$BASE/merge.sock"
LOG="$BASE/merge.log"
WSLOG="$BASE/websocket.log"
PID="$BASE/merge.pid"
WSPID="$BASE/websocket.pid"

say(){ printf '%s\n' "$*"; }
fail(){ say "[FAIL] $*"; exit 1; }
[ "$(id -u)" = 0 ] || fail "需要通过 su 以 root 执行"
[ -f "$MERGE" ] || fail "找不到 vtouchmerge: $MERGE"
[ -f "$WS" ] || fail "找不到 vtouchws: $WS"
mkdir -p "$BASE" 2>/dev/null || fail "无法创建运行目录: $BASE"

# Fast healthy-instance check; no file copy, sleep, or fixed delay.
if [ -f "$PID" ] && [ -f "$WSPID" ] && [ -S "$SOCK" ]; then
    MPID=$(cat "$PID" 2>/dev/null)
    WPID=$(cat "$WSPID" 2>/dev/null)
    case "$MPID:$WPID" in *[!0-9:]*|'') MPID=; WPID=;; esac
    if [ -n "$MPID" ] && [ -n "$WPID" ] && kill -0 "$MPID" 2>/dev/null && kill -0 "$WPID" 2>/dev/null; then
        say "[OK] VTOUCH_ALREADY_READY=1"
        exit 0
    fi
fi

# Remove stale processes without waiting; the new worker will own the new socket.
killall vtouchws 2>/dev/null || true
killall vtouchmerge 2>/dev/null || true
rm -f "$SOCK" "$PID" "$WSPID"

SIZE=$(wm size 2>/dev/null)
SIZE=${SIZE##*Physical size: }
WIDTH=${SIZE%x*}
HEIGHT=${SIZE#*x}
case "$WIDTH" in ''|*[!0-9]*) fail "无法读取显示宽度: wm size";; esac
case "$HEIGHT" in ''|*[!0-9]*) fail "无法读取显示高度: wm size";; esac

# Start both processes and return immediately. WebSocket connect/onReady is the
# runtime readiness check; no artificial sleep or polling is used here.
nohup "$MERGE" -s "$SOCK" -v 10 -w "$WIDTH" -h "$HEIGHT" >"$LOG" 2>&1 </dev/null &
MPID=$!
echo "$MPID" >"$PID"
nohup "$WS" >"$WSLOG" 2>&1 </dev/null &
WPID=$!
echo "$WPID" >"$WSPID"

# Launch commands succeeded; do not wait for socket/process readiness here.
# The WebSocket connection is the runtime-level readiness check.
say "[OK] VTOUCH_READY=1"
exit 0
