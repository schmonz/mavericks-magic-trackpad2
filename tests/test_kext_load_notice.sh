#!/bin/sh
# Tests maybe_notify_kext_stuck() (dist/scripts/kext_load_notice.sh), the postinstall's "did the kext
# really load?" check. After the loader is triggered, notify the console user to restart ONLY when:
#   - the new kext is NOT resident (it didn't come up), AND
#   - a GUI user is logged in (so it SHOULD have -- at the login window the /dev/console guard
#     legitimately defers the load to next login, which is not a stuck update).
# All externals (kextstat, console-user, notifier) are injectable, so no real kext/GUI is needed.
set -u
H="$(dirname "$0")/../dist/scripts/kext_load_notice.sh"
[ -f "$H" ] || { echo "FAIL: helper missing: $H"; exit 1; }
. "$H"
fail=0
KL_WAIT=0   # do not sleep in tests

NOTED="${TMPDIR:-/tmp}/kln.$$"
rec_notify() { echo "$1|$2" > "$NOTED"; }   # records (user, msg); same-shell function, no export needed
KL_NOTIFY=rec_notify

check() {
    desc="$1"; want="$2"
    rm -f "$NOTED"
    maybe_notify_kext_stuck "some.kext.id" "restart please" >/dev/null 2>&1
    if [ "$want" = notify ]; then
        [ -f "$NOTED" ] && echo "PASS: $desc" || { echo "FAIL: $desc (expected a notification)"; fail=1; }
    else
        [ -f "$NOTED" ] && { echo "FAIL: $desc (unexpected notification)"; fail=1; } || echo "PASS: $desc"
    fi
}

# A: kext resident -> silent (it loaded fine)
KL_KEXTSTAT="echo some.kext.id"; KL_STAT_CONSOLE="echo alice"; check "resident -> silent" silent
# B: not resident + a user logged in -> notify (it should have loaded but didn't)
KL_KEXTSTAT="echo other.kext";   KL_STAT_CONSOLE="echo alice"; check "absent + session -> notify" notify
# C: not resident + login window (console owned by root) -> silent (guard deferred by design)
KL_KEXTSTAT="echo other.kext";   KL_STAT_CONSOLE="echo root";  check "absent + login window -> silent" silent

rm -f "$NOTED"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAIL"
exit $fail
