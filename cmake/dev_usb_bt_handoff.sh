#!/bin/sh
# Install/uninstall the USB->BT handoff LaunchDaemon: a root daemon that arms an IOKit termination
# notification on com_schmonz_MT2USBReader and, on cable-unplug, wakes the deep-idle BT MT2 via
# openConnection (no physical click). Root LaunchDaemon (not a per-user Agent) so it runs regardless
# of GUI session; `|| true` tolerance is why this is a script, not inline CMake COMMANDs.
# Usage: dev_usb_bt_handoff.sh install <handoff_bin> <plist> | uninstall
set -e
DAEMON=/Library/LaunchDaemons/com.schmonz.mt2usbbthandoff.plist
BIN=/usr/local/libexec/mt2_usb_bt_handoff
case "$1" in
  install)
    sudo mkdir -p /usr/local/libexec
    sudo cp "$2" "$BIN"
    sudo cp "$3" "$DAEMON"
    sudo chown root:wheel "$DAEMON"
    sudo launchctl unload "$DAEMON" 2>/dev/null || true
    sudo launchctl load -w "$DAEMON"
    echo "Installed + loaded the mt2usbbthandoff LaunchDaemon (USB unplug now wakes BT, no tap)"
    ;;
  uninstall)
    sudo launchctl unload "$DAEMON" 2>/dev/null || true
    sudo rm -f "$DAEMON" "$BIN"
    echo "Removed the mt2usbbthandoff LaunchDaemon + binary"
    ;;
  *)
    echo "usage: $0 install <handoff_bin> <plist> | uninstall" >&2; exit 2 ;;
esac
