#!/bin/sh
# verify_migration.sh — assert a LIVE system is cleanly on the new identity with NO v0.4.5 residue.
#
# Run after a 0.4.5 -> 0.5.0 update (either the pkg-over-pkg migration test, or a real Sparkle
# auto-update) to confirm the rename teardown worked end to end. Read-only; exit 0 == clean.
#
# The genuine v0.4.5 pkg installs: com.schmonz.mt2d + com.schmonz.mt2usbbthandoff (LaunchDaemons),
# com.schmonz.mt2updatecheck + com.schmonz.mt2panewatch (LaunchAgents), the com.schmonz.MT2Gesture
# kext, and the updater app — all under /usr/local/lib/mt2d. None of that may survive the update.
set -u
fail=0
say() { printf "%-6s %s\n" "$1" "$2"; }

echo "--- 1. no legacy launchd jobs loaded ---"
for lbl in com.schmonz.mt2d com.schmonz.mt2usbbthandoff com.schmonz.mt2updatecheck \
           com.schmonz.mt2panewatch com.schmonz.voodooinputmavericks; do
    if launchctl list 2>/dev/null | grep -q "$lbl"; then say FAIL "still loaded: $lbl"; fail=1; else say ok "gone: $lbl"; fi
done

echo "--- 2. no legacy driver plists on disk (our identity only, not other com.schmonz.* products) ---"
legacy_plists=$(ls /Library/LaunchDaemons/com.schmonz.mt2* /Library/LaunchDaemons/com.schmonz.voodooinputmavericks* \
                   /Library/LaunchAgents/com.schmonz.mt2* /Library/LaunchAgents/com.schmonz.voodooinputmavericks* 2>/dev/null)
if [ -n "$legacy_plists" ]; then
    say FAIL "legacy driver plists remain:"; echo "$legacy_plists"; fail=1
else say ok "none"; fi

echo "--- 3. no legacy kext resident ---"
for k in com.schmonz.MT2Gesture com.schmonz.MT2Claim com.schmonz.MT2USBClaim \
         com.schmonz.VoodooInputMavericks com.schmonz.MavericksVoodooInputHost; do
    if kextstat 2>/dev/null | grep -q "$k"; then say FAIL "resident: $k"; fail=1; else say ok "gone: $k"; fi
done

echo "--- 4. no legacy binaries on disk (incl. the dual-loader osax + watcher + staging copy) ---"
for p in /usr/local/lib/mt2d /usr/local/share/mt2d /usr/local/sbin/mt2d-run /usr/local/sbin/mt2d \
         /usr/local/bin/mt2d /usr/local/libexec/mt2_pane_watch /usr/local/libexec/mt2_usb_bt_handoff \
         /Library/ScriptingAdditions/MT2PaneRefresh.osax; do
    if [ -e "$p" ]; then say FAIL "present: $p"; fail=1; else say ok "absent: $p"; fi
done

echo "--- 5. new identity installed ---"
[ -x /usr/local/sbin/voodooinputmavericks-run ] && say ok "loader present" || { say FAIL "loader /usr/local/sbin/voodooinputmavericks-run missing"; fail=1; }
ls /Library/LaunchDaemons/dev.modernmavericks.voodooinputmavericks*.plist >/dev/null 2>&1 \
    && say ok "new LaunchDaemons present" || { say FAIL "new LaunchDaemons missing"; fail=1; }
if kextstat 2>/dev/null | grep -qi "dev.modernmavericks.VoodooInputMavericks"; then
    say ok "new kext resident"
else
    say WARN "new kext not resident yet (loads at login via the session trigger — re-run after login + a touch)"
fi

echo
[ $fail = 0 ] && echo "RESULT: MIGRATION CLEAN" || echo "RESULT: MIGRATION RESIDUE DETECTED"
exit $fail
