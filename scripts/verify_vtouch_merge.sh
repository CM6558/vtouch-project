#!/system/bin/sh
# Verify vtouch deployment after installation.
# Read-only checks: does not start/stop processes and does not grab touch.
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
check_file() { [ -f "$1" ] && ok "file: $1" || bad "missing: $1"; }
check_exec() { [ -x "$1" ] && ok "executable: $1" || bad "not executable: $1"; }

TMP_OUT="$OUT.tmp"
{
    say "vtouch verification"
    say "time=$(date 2>/dev/null)"
    say "uid=$(id -u) context=$(id -Z 2>/dev/null)"
    say "kernel=$(uname -a)"
    say "getenforce=$(getenforce 2>/dev/null)"
    say ""

    if [ "$(id -u)" = "0" ]; then ok "root available"; else bad "not running as root"; fi

    check_exec /data/local/tmp/vtouchmerge
    check_exec /data/local/tmp/vtouchws

    if [ -c /dev/uinput ]; then ok "/dev/uinput exists"; else bad "/dev/uinput missing"; fi
    if [ -r /proc/bus/input/devices ]; then ok "input device inventory readable"; else warn "input inventory is not readable"; fi

    say ""
    say "Processes:"
    ps -A 2>/dev/null | grep -E '(^|[[:space:]])vtouch(merge|ws)([[:space:]]|$)' || true
    MRG=$(ps -A 2>/dev/null | grep -E '[ /]vtouchmerge([[:space:]]|$)' | grep -v grep | wc -l)
    WS=$(ps -A 2>/dev/null | grep -E '[ /]vtouchws([[:space:]]|$)' | grep -v grep | wc -l)
    [ "$MRG" -eq 1 ] && ok "one vtouchmerge running" || warn "vtouchmerge count=$MRG (expected 1)"
    [ "$WS" -eq 1 ] && ok "one vtouchws running" || warn "vtouchws count=$WS (expected 1)"

    say ""
    say "Runtime directory:"
    if [ -d "$RUN_BASE" ]; then ok "runtime directory exists"; else warn "runtime directory missing; service may not have run"; fi
    if [ -S "$RUN_BASE/merge.sock" ]; then ok "merge socket exists"; else warn "merge socket missing"; fi

    say ""
    say "WebSocket port:"
    SS=$(ss -ltn 2>/dev/null | grep 27183)
    if [ -n "$SS" ]; then ok "listening on 127.0.0.1:27183"; say "$SS"; else warn "port 27183 not listening"; fi

    say ""
    say "Input classification:"
    DUMPSYS=$(dumpsys input 2>/dev/null)
    printf '%s\n' "$DUMPSYS" | grep -A35 -B5 'vtouch-merged' || true
    printf '%s\n' "$DUMPSYS" | grep -q 'vtouch-merged' && ok "Android reports vtouch-merged" || warn "Android does not report vtouch-merged"

    say ""
    say "Result: pass=$PASS warn=$WARN fail=$FAIL"
    if [ "$FAIL" -eq 0 ] && [ "$MRG" -eq 1 ] && [ "$WS" -eq 1 ] && [ -S "$RUN_BASE/merge.sock" ]; then
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
