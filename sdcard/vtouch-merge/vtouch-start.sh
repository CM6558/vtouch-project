#!/system/bin/sh
# Fast start for vtouchd (single process: merger + WebSocket).
# Run: su -c 'sh /data/local/tmp/vtouch-start.sh'
set +e
BASE=${VTOUCH_BASE:-/data/local/tmp/vtouch-runtime}
BIN=${VTOUCH_BIN:-/data/local/tmp/vtouchd}
PID="$BASE/vtouchd.pid"
LOG="$BASE/vtouchd.log"

say(){ printf '%s\n' "$*"; }
fail(){ say "[FAIL] $*"; exit 1; }
[ "$(id -u)" = 0 ] || fail "需要通过 su 以 root 执行"
[ -x "$BIN" ] || fail "找不到 vtouchd: $BIN"
mkdir -p "$BASE" 2>/dev/null || fail "无法创建运行目录: $BASE"

# Healthy-instance fast path: pid alive => ready, no restart.
if [ -f "$PID" ]; then
    MPID=$(cat "$PID" 2>/dev/null)
    case "$MPID" in ''|*[!0-9]*) MPID=;; esac
    if [ -n "$MPID" ] && kill -0 "$MPID" 2>/dev/null; then
        say "[OK] VTOUCH_ALREADY_READY=1 pid=$MPID"
        exit 0
    fi
fi

# Clear stale state, including legacy dual-process leftovers.
killall vtouchd 2>/dev/null || true
killall vtouchmerge 2>/dev/null || true
killall vtouchws 2>/dev/null || true
rm -f "$BASE/vtouchd.pid" "$BASE/merge.sock" "$BASE/merge.pid" "$BASE/websocket.pid"

SIZE=$(wm size 2>/dev/null)
SIZE=${SIZE##*Physical size: }
WIDTH=${SIZE%x*}
HEIGHT=${SIZE#*x}
case "$WIDTH" in ''|*[!0-9]*) fail "无法读取显示宽度: wm size";; esac
case "$HEIGHT" in ''|*[!0-9]*) fail "无法读取显示高度: wm size";; esac

# Launch and return. WS connect is the readiness check; no sleep/poll here.
nohup "$BIN" -w "$WIDTH" -h "$HEIGHT" -p 27183 >"$LOG" 2>&1 </dev/null &
echo "$!" >"$PID"
say "[OK] VTOUCH_READY=1 pid=$(cat "$PID")"
exit 0
