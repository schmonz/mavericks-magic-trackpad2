#!/bin/sh
# Invariant: the loader daemon is WatchPaths-only (no RunAtLoad -- dropped to kill the ~8s boot throttle,
# see the loader plist). A WatchPaths job does NOT run on `launchctl load`, so the postinstall's
# `launchctl load -w` alone will NOT load the kext at install/update time. The postinstall must therefore
# actively fire the load -- by touching the session trigger (the same path login uses) or invoking the
# wrapper directly -- else a fresh install/update leaves the NEW kext UNLOADED until the next login.
#
# Regression guard for the 2026-07-23 RunAtLoad removal: without this, updating leaves the user with the
# old kext torn down (preinstall) and the new one not yet loaded.
set -u
D="$(dirname "$0")/.."
PLIST="$D/dist/dev.modernmavericks.voodooinputmavericks.plist"
POST="$D/dist/scripts/postinstall"
PRE="$D/dist/scripts/preinstall"
fail=0

# Anti-collision: IOKit registers C++ class names GLOBALLY. Our kext's classes (MT2BTReader,
# MavericksVoodooInputHost, MavericksTerminalBackend, the vendored VoodooInput/TrackpointDevice, ...)
# are shared across builds of the SAME generation, so installing a new build while a prior one is still
# resident makes the new kext FAIL to load ("a plain kext-load no-ops if a stale kext is still
# resident" -- docs/mt-stack/battery-reporting.md). The preinstall MUST unload the current kext id
# before the payload + postinstall load the new one, or a same-generation update won't load the driver.
CURRENT_KEXT="dev.modernmavericks.VoodooInputMavericks"
if grep -qE "kextunload +-b +$CURRENT_KEXT( |\$)" "$PRE"; then
    echo "PASS: preinstall unloads $CURRENT_KEXT before load (no same-generation OSMetaClass collision)"
else
    echo "FAIL: preinstall doesn't unload $CURRENT_KEXT -> a resident prior build's classes collide; new kext won't load"
    fail=1
fi

# Does the loader run at load on its own (RunAtLoad true)?
if grep -A1 "<key>RunAtLoad</key>" "$PLIST" 2>/dev/null | grep -q "<true/>"; then
    echo "PASS: loader has RunAtLoad -> load-time run covers install"
else
    # WatchPaths-only: postinstall must fire the load by touching the trigger (the same path login uses).
    # Accept a literal `touch .../session.trigger` OR the DRY form (TRIGGER=.../session.trigger; touch "$TRIGGER").
    if grep -qE "touch .*session\.trigger" "$POST" || \
       { grep -qE '[A-Za-z_]+=[^ ]*session\.trigger' "$POST" && grep -qE 'touch +"?\$\{?[A-Za-z_]+\}?"?' "$POST"; }; then
        echo "PASS: WatchPaths-only loader + postinstall touches session.trigger (kext loads at install)"
    else
        echo "FAIL: loader is WatchPaths-only but postinstall never touches session.trigger"
        echo "      -> install/update leaves the NEW kext unloaded until the next login"
        fail=1
    fi
fi
exit $fail
