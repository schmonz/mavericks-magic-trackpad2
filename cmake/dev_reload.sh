#!/bin/sh
# Hot-reload the gesture kext (ports the old Makefile `reload`): unload -> wait for our
# nub + BNB to drain (bounded; avoids the async-teardown collision) -> load -> bounce
# whichever transport is present (BT via mt2_bt_bounce, USB via mt2_reenumerate; the
# MT2 drives one transport at a time, so bouncing both is safe — the absent one no-ops).
# Args: <kext_bundle> <bt_bounce_bin> <reenumerate_bin>
set -e
KEXT="$1"; BT_BOUNCE="$2"; REENUM="$3"

sudo kextunload -b com.schmonz.MT2Gesture || true
echo "reload: waiting for our nub + BNB to drain (async teardown)..."
for i in $(seq 1 50); do
  ioreg -lw0 | grep -q '"com_schmonz_MT2Gesture"=1\|"BNBTrackpadDevice"=1' || break
  sleep 0.1
done

sudo rm -rf /tmp/MT2Gesture.kext
sudo cp -R "$KEXT" /tmp/
sudo chown -R root:wheel /tmp/MT2Gesture.kext
# Load Apple's multitouch kexts first (best-effort); the genuine paths manual-start them.
sudo kextload /System/Library/Extensions/AppleUSBMultitouch.kext || true
sudo kextload /System/Library/Extensions/AppleBluetoothMultitouch.kext || true
sudo kextload /tmp/MT2Gesture.kext

echo "reload: bouncing present transport(s) for a clean re-establish..."
"$BT_BOUNCE" || true
"$REENUM" || true
