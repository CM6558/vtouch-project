#!/system/bin/sh
# Verify VTouch Merge KernelSU module after installation/reboot.
# Read-only checks: does not start/stop processes and does not grab touch.
set +e

MOD_ID="vtouch-merge"
MOD_BASE="/data/adb/modules/$MOD_ID"
RUN_BASE="/data/adb/vtouch-merge"
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
    say "VTouch Merge verification"
    say "time=$(date 2>/dev/null)"
    say "uid=$(id -u) context=$(id -Z 2>/dev/null)"
    say "kernel=$(uname -a)"
    say "getenforce=$(getenforce 2>/dev/null)"
    say ""

    if [ "$(id -u)" = "0" ]; then ok "root available"; else bad "not running as root"; fi
    if [ -d "$MOD_BASE" ]; then ok "KernelSU module directory exists: $MOD_BASE"; else bad "module directory missing: $MOD_BASE"; fi
    if [ -f "$MOD_BASE/disable" ]; then bad "module is disabled"; else ok "module is enabled"; fi

    check_file "$MOD_BASE/module.prop"
    check_file "$MOD_BASE/service.sh"
    check_exec "$MOD_BASE/system/bin/vtouchmerge"
    check_exec "$MOD_BASE/system/bin/vtouchsupervise"

    if [ -c /dev/uinput ]; then ok "/dev/uinput exists"; else bad "/dev/uinput missing"; fi
    if [ -r /proc/bus/input/devices ]; then ok "input device inventory readable"; else warn "input inventory is not readable"; fi

    say ""
    say "Processes:"
    ps -A 2>/dev/null | grep -E '(^|[[:space:]])vtouch(supervise|merge)([[:space:]]|$)' || true
    SUP=$(ps -A 2>/dev/null | grep -E '[ /]vtouchsupervise([[:space:]]|$)' | grep -v grep | wc -l)
    WRK=$(ps -A 2>/dev/null | grep -E '[ /]vtouchmerge([[:space:]]|$)' | grep -v grep | wc -l)
    [ "$SUP" -eq 1 ] && ok "one supervisor running" || warn "supervisor count=$SUP (expected 1)"
    [ "$WRK" -eq 1 ] && ok "one merge worker running" || warn "worker count=$WRK (expected 1)"

    say ""
    say "Runtime directory:"
    if [ -d "$RUN_BASE" ]; then ok "runtime directory exists"; else warn "runtime directory missing; service may not have run"; fi
    if [ -S "$RUN_BASE/merge.sock" ]; then ok "merge socket exists"; else warn "merge socket missing"; fi
    if [ -f "$RUN_BASE/supervisor.log" ]; then
        ok "supervisor log exists"
        say "--- last log lines ---"
        tail -30 "$RUN_BASE/supervisor.log" 2>/dev/null || true
        say "--- end log ---"
    else
        warn "supervisor log missing"
    fi

    say ""
    say "Input classification:"
    DUMPSYS=$(dumpsys input 2>/dev/null)
    printf '%s\n' "$DUMPSYS" | grep -A35 -B5 'vtouch-merged' || true
    printf '%s\n' "$DUMPSYS" | grep -q 'vtouch-merged' && ok "Android reports vtouch-merged" || warn "Android does not report vtouch-merged"

    say ""
    say "Result: pass=$PASS warn=$WARN fail=$FAIL"
    if [ "$FAIL" -eq 0 ] && [ "$SUP" -eq 1 ] && [ "$WRK" -eq 1 ] && [ -S "$RUN_BASE/merge.sock" ]; then
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
