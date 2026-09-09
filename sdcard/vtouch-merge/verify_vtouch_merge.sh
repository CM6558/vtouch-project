#!/system/bin/sh
# Verify vtouchd deployment. Read-only: does not start/stop or grab touch.
set +e

RUN_BASE="/data/local/tmp/vtouch-runtime"
OUT="${1:-/sdcard/vtouch-merge-verify.txt}"
PASS=0
WARN=0
FAIL=0

say() { printf '%s\n' "$*"; }
ok() { PASS=$((PASS + 1)); say "[PASS] $*"; }
warn() { WARN=$((WARN + 1)); say "[WARN] $*"; }
bad() { FAIL=$((FAIL + 1)); say "[FAIL] $*"; }

TMP_OUT="$OUT.tmp"
{
    say "vtouchd verification"
    say "time=$(date 2>/dev/null)"
    say "uid=$(id -u) context=$(id -Z 2>/dev/null)"

    if [ "$(id -u)" = "0" ]; then ok "root available"; else bad "not running as root"; fi

    [ -x /data/local/tmp/vtouchd ] && ok "executable: /data/local/tmp/vtouchd" || bad "not executable: /data/local/tmp/vtouchd"
    [ -c /dev/uinput ] && ok "/dev/uinput exists" || bad "/dev/uinput missing"

    say ""
    say "Process:"
    N=$(ps -A 2>/dev/null | grep -E '[ /]vtouchd([[:space:]]|$)' | grep -v grep | wc -l)
    [ "$N" -eq 1 ] && ok "one vtouchd running" || warn "vtouchd count=$N (expected 1)"
    [ -f "$RUN_BASE/vtouchd.pid" ] && ok "pid file exists" || warn "pid file missing"
    if [ -f "$RUN_BASE/vtouchd.pid" ]; then
        P=$(cat "$RUN_BASE/vtouchd.pid" 2>/dev/null)
        kill -0 "$P" 2>/dev/null && ok "pid $P alive" || warn "pid $P not alive"
    fi

    say ""
    say "WebSocket port:"
    SS=$(ss -ltn 2>/dev/null | grep 27183)
    if [ -n "$SS" ]; then ok "listening on 127.0.0.1:27183"; say "$SS"; else warn "port 27183 not listening"; fi

    say ""
    say "Input classification:"
    if dumpsys input 2>/dev/null | grep -q 'vtouch-merged'; then
        ok "Android reports vtouch-merged"
    else
        warn "Android does not report vtouch-merged"
    fi

    say ""
    say "Result: pass=$PASS warn=$WARN fail=$FAIL"
    if [ "$FAIL" -eq 0 ] && [ "$N" -eq 1 ] && [ -n "$SS" ]; then
        say "RESULT=READY_FOR_STAGED_TOUCH_TEST"
        exit 0
    fi
    if [ "$FAIL" -eq 0 ]; then
        say "RESULT=INSTALLED_BUT_NOT_READY"
        exit 2
    fi
    say "RESULT=FAILED"
    exit 1
} 2>&1 | tee "$TMP_OUT"
RC=${PIPESTATUS:-1}
cat "$TMP_OUT" > "$OUT" 2>/dev/null || true
rm -f "$TMP_OUT"
exit "$RC"
