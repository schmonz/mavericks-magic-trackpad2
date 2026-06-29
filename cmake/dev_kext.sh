#!/bin/sh
# Kext load/unload helper for the CMake kext-load / kext-unload targets (ports the old
# kext-gesture/Makefile load/unload). Kept in a script so the multi-step load sequence
# and the tolerant (|| true) steps stay readable — CMake's COMMAND can't express
# `a && b || true` chains without the sh -c space-escaping bug (Phase-0 finding).
# Usage: dev_kext.sh load <kext_bundle> | dev_kext.sh unload
set -e
case "$1" in
  load)
    KEXT="$2"
    sudo rm -rf /tmp/MT2Gesture.kext
    sudo cp -R "$KEXT" /tmp/
    sudo chown -R root:wheel /tmp/MT2Gesture.kext
    # Apple's multitouch kexts first (best-effort): the genuine paths allocClassWithName
    # AppleUSBMultitouchDriver / BNBTrackpadDevice from these (NULL class otherwise).
    sudo kextload /System/Library/Extensions/AppleUSBMultitouch.kext || true
    sudo kextload /System/Library/Extensions/AppleBluetoothMultitouch.kext || true
    sudo kextload /tmp/MT2Gesture.kext
    ;;
  unload)
    sudo kextunload -b com.schmonz.MT2Gesture || true
    ;;
  *)
    echo "usage: $0 load <kext_bundle> | unload" >&2; exit 2 ;;
esac
