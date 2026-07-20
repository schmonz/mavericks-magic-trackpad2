#!/bin/sh
# Fail the build if a shipped Info.plist is malformed, or the updater ships with a
# broken self-update configuration.
#
# Two invariants:
#   (1) Every shipped Info.plist that exists must pass `plutil -lint`. A malformed plist
#       means the osax/SIMBL/kext/updater bundle won't load (or will load with a nil
#       Info dictionary) on the target machine.
#   (2) If the updater app is present, its Info.plist MUST carry a non-empty SUFeedURL
#       (else Sparkle can't find the appcast -> no updates) AND a non-empty SUPublicEDKey
#       (else Sparkle can't verify a downloaded update -> refuses/insecure). Shipping the
#       updater without either is worse than not shipping it.
#
# Fail-closed: if zero plists were found to lint, that's a broken/empty build, not a pass.
#
# Usage: check_plists.sh <build-dir>
set -eu
BUILD=$1

PLISTS="
$BUILD/MT2PaneRefresh.osax/Contents/Info.plist
$BUILD/MT2PaneRefresh.bundle/Contents/Info.plist
$BUILD/MavericksVoodooInputHost.kext/Contents/Info.plist
$BUILD/MavericksTrackpad2Updater.app/Contents/Info.plist
"

fail=0
linted=0
for p in $PLISTS; do
  [ -f "$p" ] || continue
  linted=$((linted + 1))
  if ! plutil -lint "$p" >/dev/null 2>&1; then
    echo "check_plists: MALFORMED plist $p" >&2
    plutil -lint "$p" >&2 || true
    echo "  fix: repair the plist syntax (plutil -lint names the error)." >&2
    fail=1
  fi
done

# Updater self-update config: SUFeedURL + SUPublicEDKey must be present and non-empty.
UPD_PLIST="$BUILD/MavericksTrackpad2Updater.app/Contents/Info.plist"
if [ -f "$UPD_PLIST" ]; then
  for key in SUFeedURL SUPublicEDKey; do
    val=$(/usr/libexec/PlistBuddy -c "Print :$key" "$UPD_PLIST" 2>/dev/null || true)
    if [ -z "$val" ]; then
      echo "check_plists: updater $UPD_PLIST is missing/empty $key." >&2
      echo "  fix: an updater without $key can't $( [ "$key" = SUFeedURL ] && echo 'check for' || echo 'verify' ) updates -- set it before shipping." >&2
      fail=1
    fi
  done
fi

# Fail closed.
[ "$linted" -gt 0 ] || { echo "check_plists: no shipped plists found under $BUILD (fail-closed)" >&2; exit 2; }

[ "$fail" = 0 ] && echo "check_plists: $linted shipped plist(s) valid$( [ -f "$UPD_PLIST" ] && echo ' + updater self-update config present')" || exit 1
