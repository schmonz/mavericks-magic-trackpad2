#!/bin/sh
# Invariant: the pkg install scripts carry NO legacy-identity (com.schmonz) migration. None of these
# products shipped under the old com.schmonz.* identities long enough to need user migration, so the
# rename teardown and the updater "check automatically" carry-over (migrate_autocheck.sh) were removed.
# This guards them from creeping back. What the preinstall MUST still do: unload the CURRENT daemons so
# the payload can replace their binaries (the kext itself is not hot-unloaded -- see
# test_install_kext_load.sh).
set -u
DIR="$(dirname "$0")/../dist/scripts"
PRE="$DIR/preinstall"
POST="$DIR/postinstall"
fail=0
pass() { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

[ -f "$PRE" ]  || bad "preinstall missing"
[ -f "$POST" ] || bad "postinstall missing"

# Kept: preinstall unloads the current daemons before the payload replaces them.
for lbl in dev.modernmavericks.voodooinputmavericks dev.modernmavericks.voodooinputmavericks.linkstated; do
  if grep -q "launchctl unload /Library/LaunchDaemons/$lbl.plist" "$PRE"; then
    pass "preinstall unloads $lbl"
  else
    bad "preinstall no longer unloads the current daemon $lbl"
  fi
done

# Removed: no com.schmonz rename teardown, no autocheck migration, in EITHER script.
for f in "$PRE" "$POST"; do
  b=$(basename "$f")
  grep -q 'com\.schmonz'      "$f" && bad "$b still references com.schmonz (legacy identity migration)" || pass "$b free of com.schmonz"
  grep -q 'migrate_autocheck' "$f" && bad "$b still references migrate_autocheck"                        || pass "$b free of migrate_autocheck"
done

# Removed: the autocheck-migration helper file itself.
[ -e "$DIR/migrate_autocheck.sh" ] && bad "migrate_autocheck.sh still present" || pass "migrate_autocheck.sh removed"

[ "$fail" = 0 ] && echo "ALL PASS: install scripts free of legacy migration" || echo "LEGACY MIGRATION STILL PRESENT"
exit $fail
