#!/bin/sh
# Invariant: the pkg preinstall must tear down EVERY identity any prior shipped release installed, so
# a rename-release update (com.schmonz.* -> dev.modernmavericks.*) never leaves a duplicate kext
# resident or a stale LaunchDaemon/Agent relaunching a removed binary.
#
# Regression guard for the 2026-07-23 audit: v0.4.5's ACTUAL gesture kext (com.schmonz.MT2Gesture) was
# absent from the kextunload list, and its updatecheck/panewatch agent plists were never removed, so a
# 0.4.5->0.5.0 auto-update could leave the old kext loaded + stale agents behind. "Fix the class, not the
# instance": assert the whole legacy identity set is covered, not just the one we happened to notice.
set -u
PRE="$(dirname "$0")/../dist/scripts/preinstall"
fail=0

# Fold backslash line-continuations so a multi-file `rm a \<newline> b` block reads as one logical line
# (else a plist on a continuation line is invisible to a per-line grep).
NORM="$(awk '{ while (sub(/\\[[:space:]]*$/,"")) { if ((getline nxt) > 0) $0 = $0 nxt; else break } print }' "$PRE")"

# Legacy KEXT bundle ids -> must be kextunload'd (else the old kext stays resident beside the new one
# until reboot, both claiming the MT2).
for id in com.schmonz.MT2Gesture com.schmonz.VoodooInputMavericks com.schmonz.MavericksVoodooInputHost \
          com.schmonz.MT2Claim com.schmonz.MT2USBClaim; do
    if echo "$NORM" | grep -qE "kextunload +-b +$id( |\$)"; then
        echo "PASS: kextunload $id"
    else
        echo "FAIL: preinstall never kextunloads legacy kext $id"; fail=1
    fi
done

# Legacy LaunchDaemon/Agent plists -> must be rm'd (unload alone leaves the .plist to relaunch at the
# next boot, pointing at a now-removed binary).
for lbl in com.schmonz.mt2d com.schmonz.mt2updatecheck com.schmonz.mt2panewatch com.schmonz.mt2usbbthandoff; do
    if echo "$NORM" | grep -E "rm " | grep -qF "$lbl.plist"; then
        echo "PASS: rm $lbl.plist"
    else
        echo "FAIL: preinstall never rm's $lbl.plist (unload alone lets it relaunch)"; fail=1
    fi
done

# Legacy loader binaries -> must be rm'd.
for bin in mt2d-run mt2d; do
    if echo "$NORM" | grep -E "rm " | grep -qE "/$bin( |\$)"; then
        echo "PASS: rm legacy loader $bin"
    else
        echo "FAIL: preinstall never rm's the legacy loader $bin"; fail=1
    fi
done

if [ "$fail" = 0 ]; then echo "ALL PASS: legacy identity teardown complete"; else echo "MIGRATION TEARDOWN INCOMPLETE"; fi
exit $fail
