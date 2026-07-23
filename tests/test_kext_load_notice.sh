#!/bin/sh
# Tests maybe_notify_user() (dist/scripts/kext_load_notice.sh): show a message to the logged-in GUI user,
# but stay silent at the login window (console owned by root) where there's no one to tell. Used by the
# postinstall to say "restart to finish" when an update is staged (a running driver can't be hot-swapped,
# so the new kext applies at the next boot). Externals injected; no real GUI needed.
set -u
H="$(dirname "$0")/../dist/scripts/kext_load_notice.sh"
[ -f "$H" ] || { echo "FAIL: helper missing: $H"; exit 1; }
. "$H"
fail=0

NOTED="${TMPDIR:-/tmp}/kln.$$"
rec_notify() { echo "$1|$2" > "$NOTED"; }   # records (user, msg); same-shell function
KL_NOTIFY=rec_notify

check() {
    desc="$1"; want="$2"
    rm -f "$NOTED"
    maybe_notify_user "restart please" >/dev/null 2>&1
    if [ "$want" = notify ]; then
        [ -f "$NOTED" ] && echo "PASS: $desc" || { echo "FAIL: $desc (expected a notification)"; fail=1; }
    else
        [ -f "$NOTED" ] && { echo "FAIL: $desc (unexpected notification)"; fail=1; } || echo "PASS: $desc"
    fi
}

# A GUI user is logged in -> notify them.
KL_STAT_CONSOLE="echo alice"; check "session -> notify" notify
# Login window (console owned by root) -> no one to tell -> silent.
KL_STAT_CONSOLE="echo root";  check "login window -> silent" silent

rm -f "$NOTED"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAIL"
exit $fail
