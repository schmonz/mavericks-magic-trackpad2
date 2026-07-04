#!/bin/sh
# Install/uninstall the BT-name mirror LaunchAgent: an agent that WatchPaths the Bluetooth prefs and,
# when the user renames the MT2 in the pane (which writes only the host `displayName` alias), pushes
# that name ONBOARD via `mt2_set_btname --mirror` (-[BluetoothHIDDevice setDeviceName:]) so the rename
# follows the device. Mirrors cmake/dev_prefpane_watch.sh: launchctl runs as the invoking GUI-session
# user; file ops use sudo; `|| true` tolerance is why this is a script, not inline CMake COMMANDs.
# Usage: dev_name_mirror.sh install <btname_bin> <plist> | uninstall
set -e
AGENT=/Library/LaunchAgents/com.schmonz.mt2namemirror.plist
BIN=/usr/local/libexec/mt2_set_btname
case "$1" in
  install)
    sudo mkdir -p /usr/local/libexec
    sudo cp "$2" "$BIN"
    sudo cp "$3" "$AGENT"
    launchctl unload "$AGENT" 2>/dev/null || true
    launchctl load -w "$AGENT"
    echo "Installed + loaded the mt2namemirror LaunchAgent (renames now follow the device onboard)"
    ;;
  uninstall)
    launchctl unload "$AGENT" 2>/dev/null || true
    sudo rm -f "$AGENT"
    # Leave $BIN in place: mt2_set_btname is also a standalone tool (manual name-set + mirror).
    echo "Removed the mt2namemirror LaunchAgent (mt2_set_btname binary left in place)"
    ;;
  *)
    echo "usage: $0 install <btname_bin> <plist> | uninstall" >&2; exit 2 ;;
esac
