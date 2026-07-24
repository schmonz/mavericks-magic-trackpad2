#!/bin/sh
# Behavior test for dist/scripts/migrate_autocheck.sh: the Sparkle "check automatically" opt-in must carry
# across an updater identity rename -- copied from the old per-user domain to the new one -- but ONLY when
# the new domain has no explicit value yet (never clobber a new-identity choice) and the old domain set
# one. Regression guard for the 2026-07-24 finding that the com.schmonz -> dev.modernmavericks rename
# silently reset a user's enabled auto-check to the off default.
#
# Exercises the real script through throwaway `defaults` domains for THIS user (never the shipping ones);
# cleans them up on exit.
set -u
MIG="$(dirname "$0")/../dist/scripts/migrate_autocheck.sh"
OLD="dev.modernmavericks.test-autocheck-old.$$"
NEW="dev.modernmavericks.test-autocheck-new.$$"
KEY=SUEnableAutomaticChecks
fail=0

reset()   { defaults delete "$OLD" >/dev/null 2>&1 || true; defaults delete "$NEW" >/dev/null 2>&1 || true; }
trap reset EXIT
val()     { defaults read "$1" "$KEY" 2>/dev/null || echo unset; }   # -> 1 | 0 | unset
check()   { if [ "$2" = "$3" ]; then echo "PASS: $1 (=$3)"; else echo "FAIL: $1 (expected $2, got $3)"; fail=1; fi; }

# 1. old ON, new unset -> carries ON
reset; defaults write "$OLD" "$KEY" -bool true
sh "$MIG" "$OLD" "$NEW"; check "opt-in ON carries to the new domain" 1 "$(val "$NEW")"

# 2. old OFF, new unset -> carries OFF (an explicit opt-out is a real choice worth preserving)
reset; defaults write "$OLD" "$KEY" -bool false
sh "$MIG" "$OLD" "$NEW"; check "opt-out OFF carries to the new domain" 0 "$(val "$NEW")"

# 3. old ON, new already OFF -> new stays OFF (never clobber a choice made under the new identity)
reset; defaults write "$OLD" "$KEY" -bool true; defaults write "$NEW" "$KEY" -bool false
sh "$MIG" "$OLD" "$NEW"; check "existing new-domain choice is not clobbered" 0 "$(val "$NEW")"

# 4. old unset, new unset -> new stays unset (nothing to carry; leave the built-in default)
reset
sh "$MIG" "$OLD" "$NEW"; check "no old value -> new stays unset" unset "$(val "$NEW")"

# 5. idempotent: a second run changes nothing
reset; defaults write "$OLD" "$KEY" -bool true
sh "$MIG" "$OLD" "$NEW"; sh "$MIG" "$OLD" "$NEW"; check "second run is a no-op" 1 "$(val "$NEW")"

[ "$fail" = 0 ] && echo "ALL PASS: autocheck opt-in migration" || echo "AUTOCHECK MIGRATION BROKEN"
exit $fail
