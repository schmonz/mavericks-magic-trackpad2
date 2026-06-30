#!/bin/sh
# Install/uninstall the prefpane-refresh launch-watcher (Branch A of the standalone-osax
# delivery): the watcher binary + its per-user LaunchAgent. Mirrors what the pkg does, for
# dev/manual use. Does NOT touch SIMBL (users keep SIMBL for unrelated plugins; we ship no
# SIMBL plugin). launchctl load/unload run as the invoking (GUI-session) user; file ops use
# sudo. The `|| true` tolerance is why this is a script, not inline CMake COMMANDs.
# Usage: dev_prefpane_watch.sh install <watcher_bin> <plist> | uninstall
set -e
AGENT=/Library/LaunchAgents/com.schmonz.mt2panewatch.plist
BIN=/usr/local/libexec/mt2_pane_watch
case "$1" in
  install)
    sudo mkdir -p /usr/local/libexec
    sudo cp "$2" "$BIN"
    sudo cp "$3" "$AGENT"
    launchctl unload "$AGENT" 2>/dev/null || true
    launchctl load -w "$AGENT"
    echo "Installed + loaded the mt2panewatch LaunchAgent"
    ;;
  uninstall)
    launchctl unload "$AGENT" 2>/dev/null || true
    sudo rm -f "$AGENT" "$BIN"
    echo "Removed the mt2panewatch LaunchAgent + watcher binary"
    ;;
  *)
    echo "usage: $0 install <watcher_bin> <plist> | uninstall" >&2; exit 2 ;;
esac
