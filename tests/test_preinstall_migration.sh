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

# Model A defers the KEXT swap to reboot -- a kext driving the live MT2 can't be hot-unloaded (partial
# teardown kills the trackpad; see test_install_kext_load.sh). So migration completeness here means the
# old LOADERS + FILES are removed, so nothing re-loads the old identity at that boot -- NOT that we
# kextunload a live driver. v0.4.5's gesture kext + updater live under /usr/local/lib/mt2d.
if echo "$NORM" | grep -E "rm " | grep -qE "/usr/local/lib/mt2d( |\$)"; then
    echo "PASS: rm /usr/local/lib/mt2d (old kext + updater tree -> can't reload at boot)"
else
    echo "FAIL: preinstall never removes /usr/local/lib/mt2d -> old kext could reload at boot"; fail=1
fi

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

# Pane DUAL-LOADER teardown. v0.4.5's prefpane companion shipped under its PRE-RENAME name via TWO loaders
# plus a staging copy + link-state binary (authoritative: `pkgutil --files com.schmonz.mt2d`). The
# 2026-07-24 regression: the osax block purged only the intermediate dev-identity names
# (VoodooInputMavericksPane.osax / voodooinputmavericks_pane_watch), so genuine v0.4.5's MT2PaneRefresh.osax
# survived and co-loaded a SECOND, stale About pane into System Preferences. Assert the whole genuine-0.4.5
# set is torn down -- these exact paths came from the shipped 0.4.5 receipt, so this is class-level coverage.
for path in \
    "/Library/ScriptingAdditions/MT2PaneRefresh.osax" \
    "/usr/local/libexec/mt2_pane_watch" \
    "/usr/local/libexec/mt2_usb_bt_handoff" \
    "/usr/local/share/mt2d"; do
    if echo "$NORM" | grep -E "rm " | grep -qF "$path"; then
        echo "PASS: rm $path"
    else
        echo "FAIL: preinstall never rm's $path (genuine-0.4.5 residue survives the rename update)"; fail=1
    fi
done

if [ "$fail" = 0 ]; then echo "ALL PASS: legacy identity teardown complete"; else echo "MIGRATION TEARDOWN INCOMPLETE"; fi
exit $fail
