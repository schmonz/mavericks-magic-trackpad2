#!/bin/sh
# Tests the mt2d-run boot wrapper's sentinel state machine in dry-run mode.
# Dry-run (MT2D_DRYRUN=1) decides MODE and updates the state file but performs
# no kextload/exec, so the brick-guard logic is testable without a real panic.
set -u
WRAPPER="$(dirname "$0")/../dist/mt2d-run"
TMP="$(mktemp -d -t mt2d_run_test)"
trap 'rm -rf "$TMP"' EXIT
fail=0

check() {
    desc="$1"; want_mode="$2"; want_state="$3"; init_state="$4"
    sf="$TMP/state"
    if [ "$init_state" = "MISSING" ]; then rm -f "$sf"; else echo "$init_state" > "$sf"; fi
    out="$(MT2D_STATE_FILE="$sf" MT2D_DRYRUN=1 "$WRAPPER" 2>/dev/null)"
    got_mode="$(echo "$out" | sed -n 's/^MODE=//p')"
    got_state="$(echo "$out" | sed -n 's/^STATE=//p')"
    if [ "$got_mode" = "$want_mode" ] && [ "$got_state" = "$want_state" ]; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc (mode=$got_mode want $want_mode; state=$got_state want $want_state)"
        fail=1
    fi
}

check "missing state -> full/trying"  full  trying  MISSING
check "ok          -> full/trying"    full  trying  ok
check "trying      -> skip/trying"    skip  trying  trying

# --reset writes ok regardless of prior state
sf="$TMP/state"; echo trying > "$sf"
MT2D_STATE_FILE="$sf" "$WRAPPER" --reset >/dev/null 2>&1
if [ "$(cat "$sf")" = "ok" ]; then echo "PASS: --reset -> ok"; else echo "FAIL: --reset (got $(cat "$sf"))"; fail=1; fi

if [ $fail -eq 0 ]; then echo "ALL mt2d-run TESTS PASS"; else echo "mt2d-run TESTS FAILED"; exit 1; fi
exit 0
