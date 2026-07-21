#!/bin/sh
# Install/uninstall the mt2_bluetooth_linkstated LaunchDaemon (the MT2's Bluetooth link-state daemon:
# reconnect keeper + USB-removal handoff + USB-appear yield + shutdown quiesce). Root LaunchDaemon (not a
# per-user Agent) so it runs regardless of GUI session; the `|| true` tolerance is why this is a script,
# not inline CMake COMMANDs.
# Usage: dev_bluetooth_linkstated.sh install <daemon_bin> <plist> | uninstall
set -e
DAEMON=/Library/LaunchDaemons/com.schmonz.voodooinputmavericks.bluetoothlinkstated.plist
BIN=/usr/local/libexec/mt2_bluetooth_linkstated
case "$1" in
  install)
    sudo mkdir -p /usr/local/libexec
    sudo cp "$2" "$BIN"
    sudo cp "$3" "$DAEMON"
    sudo chown root:wheel "$DAEMON"
    sudo launchctl unload "$DAEMON" 2>/dev/null || true
    sudo launchctl load -w "$DAEMON"
    echo "Installed + loaded the mt2_bluetooth_linkstated LaunchDaemon"
    ;;
  uninstall)
    sudo launchctl unload "$DAEMON" 2>/dev/null || true
    sudo rm -f "$DAEMON" "$BIN"
    echo "Removed the mt2_bluetooth_linkstated LaunchDaemon + binary"
    ;;
  *)
    echo "usage: $0 install <daemon_bin> <plist> | uninstall" >&2; exit 2 ;;
esac
