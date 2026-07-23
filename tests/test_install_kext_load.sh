#!/bin/sh
# Model A -- stage-and-apply-at-reboot. A kext driving the live MT2 cannot be hot-swapped in place (its
# terminal holds a synthetic IOHIDDevice the HID stack retains, like upstream VoodooInput's simulator),
# so the install must NOT force-unload a running kext: kextunload does a PARTIAL teardown that kills the
# reader (trackpad dead) then fails, and the same-id new kext can't load over the stuck image -> dead
# until reboot (observed 2026-07-23). Instead: fresh install loads now; an update stages the new kext,
# leaves the old one driving, and notifies restart; the new kext applies cleanly at the next boot.
set -u
D="$(dirname "$0")/.."
PLIST="$D/dist/dev.modernmavericks.voodooinputmavericks.plist"
POST="$D/dist/scripts/postinstall"
PRE="$D/dist/scripts/preinstall"
fail=0

# 1. Preinstall must NOT kextunload a (possibly-running) kext -- that is the partial-teardown breakage.
if grep -qE "^[^#]*kextunload" "$PRE"; then
    echo "FAIL: preinstall runs kextunload -> can partial-tear-down a live driver (dead-until-reboot)"; fail=1
else
    echo "PASS: preinstall does not force-unload a running kext"
fi

# 2. Postinstall gates the load: fire the WatchPaths trigger only when NO prior kext is resident (fresh
#    install); on an update it stages silently (the Trackpad pane surfaces the restart nudge -- no dialog).
if grep -q "kextstat" "$POST" && grep -q 'touch "$TRIGGER"' "$POST"; then
    echo "PASS: postinstall gates load on kextstat -> touch trigger only on a fresh install"
else
    echo "FAIL: postinstall must gate on kextstat and touch the trigger on a fresh install"; fail=1
fi

# 3. Loader stays WatchPaths-only (no RunAtLoad -> avoids the ~8s boot throttle); the fresh-install touch
#    is what fires it, since a WatchPaths job does not run at `launchctl load`.
if grep -A1 "<key>RunAtLoad</key>" "$PLIST" 2>/dev/null | grep -q "<true/>"; then
    echo "FAIL: loader has RunAtLoad (reintroduces the boot throttle)"; fail=1
else
    echo "PASS: loader is WatchPaths-only"
fi

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAIL"
exit $fail
