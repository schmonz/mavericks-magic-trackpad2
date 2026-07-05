#!/bin/sh
# mt2_verify_prefpane.sh — one-pass on-device verification of the prefpane / BT-UI features.
#
# Runs every AUTOMATABLE data-layer check for the battery / icon / button work (they read the
# global IOKit registry + syslog, so a script can assert them), auto-adapting to the current
# transport, and prints the short MANUAL visual steps a script can't see. A `watch` mode samples
# state during a physical USB<->BT transition so the hand-off checks get captured too.
#
# Usage:
#   tools/mt2_verify_prefpane.sh          # one-pass snapshot of the current state
#   tools/mt2_verify_prefpane.sh watch    # live monitor (cycle the cable; Ctrl-C to stop)
#
# Reads only (ioreg / syslog / GET_REPORT probes); never writes the device or the registry.
# Uses sudo for the syslog reads (osax injection log lives in /var/log/system.log).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=/tmp/mt2verify; mkdir -p "$BIN"
PASS=0; FAIL=0; SKIP=0
green() { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
red()   { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
skip()  { printf '  \033[33mSKIP\033[0m %s\n' "$1"; SKIP=$((SKIP+1)); }
info()  { printf '  ---- %s\n' "$1"; }

# Build a probe from tools/ into $BIN if missing/stale. $1=src $2=out $3=extra-frameworks
build() {
    src=$ROOT/tools/$1; out=$BIN/$2
    [ -x "$out" ] && [ "$out" -nt "$src" ] && return 0
    # shellcheck disable=SC2086
    clang -O2 -o "$out" "$src" -framework IOKit -framework CoreFoundation $3 2>/dev/null
}

# ---- current transport ------------------------------------------------------------------------
bt_count()  { ioreg -c BNBTrackpadDevice -w0 2>/dev/null | grep -c "class BNBTrackpadDevice,"; }
usb_count() { ioreg -c AppleUSBMultitouchDriver -w0 2>/dev/null | grep -c "class AppleUSBMultitouchDriver,"; }
sysprefs_pid() { ps ax -o pid,comm 2>/dev/null | grep -i "System Preferences.app" | grep -v grep | awk '{print $1}' | head -1; }

transport() {
    if [ "$(bt_count)" -ge 1 ]; then echo BT
    elif [ "$(usb_count)" -ge 1 ]; then echo USB
    else echo NONE; fi
}

# ---- one snapshot -----------------------------------------------------------------------------
snapshot() {
    T=$(transport)
    echo "== MT2 prefpane verification =="
    echo "transport: $T   (BNBTrackpadDevice=$(bt_count)  AppleUSBMultitouchDriver=$(usb_count))"
    echo

    build mt2_battery_probe.c      batt       ""
    build mt2_hidnode_probe.c      hidnode    ""
    build mt2_panebattery_probe.m  pane       "-framework Foundation -framework IOBluetooth -lobjc"

    echo "[1] Raw device battery (GET_REPORT 0x90; either transport)"
    if [ -x "$BIN/batt" ]; then
        out=$("$BIN/batt" 2>&1)
        if echo "$out" | grep -q "battery:"; then green "$out"; else red "no read ($out)"; fi
    else red "probe build failed (need Xcode CLT)"; fi

    echo "[2] Battery node (BT only): BatteryPercent + ExtendedFeatures on BNBTrackpadDevice"
    if [ "$T" = BT ]; then
        bp=$(ioreg -r -c BNBTrackpadDevice -w0 2>/dev/null | grep '"BatteryPercent"' | grep -oE '[0-9]+$')
        ef=$(ioreg -r -c BNBTrackpadDevice -w0 2>/dev/null | grep -c '"ExtendedFeatures"')
        [ -n "$bp" ] && green "BatteryPercent=$bp" || red "BatteryPercent absent (poll not run yet? wait ~5s)"
        [ "$ef" -ge 1 ] && green "ExtendedFeatures present (pane/menu-extra gate)" || red "ExtendedFeatures absent -> pane+menu show 0%"
    else skip "not on BT (battery UI is BT-side)"; fi

    echo "[3] Pane + menu-extra data path: [AppleBluetoothHIDDevice withBluetoothDevice:] batteryPercent"
    if [ "$T" = BT ] && [ -x "$BIN/pane" ]; then
        pout=$("$BIN/pane" 2>&1)
        bpc=$(echo "$pout" | grep -oE "batteryPercent -> [0-9.]+" | awk '{print $3}')
        if echo "$pout" | grep -q "class AppleBluetoothHIDDevice"; then
            green "withBluetoothDevice: -> live wrapper; batteryPercent=$bpc (pane shows ${bpc}0%; menu extra same path)"
        else red "withBluetoothDevice: -> nil (ExtendedFeatures gate?) -> both would show 0%"; fi
    elif [ "$T" != BT ]; then skip "not on BT"
    else red "pane probe build failed"; fi

    echo "[4] HID-node match: BNBTrackpadDevice in IOServiceMatching(\"IOBluetoothHIDDriver\") w/ BD_ADDR"
    if [ "$T" = BT ] && [ -x "$BIN/hidnode" ]; then
        hout=$("$BIN/hidnode" 2>&1)
        echo "$hout" | grep -q "class = BNBTrackpadDevice" && green "resolves to our node" || red "no match ($hout)"
    else skip "not on BT (or probe missing)"; fi

    echo "[5] osax injected + prefpane paints (needs System Prefs open on the Trackpad/Bluetooth pane)"
    PID=$(sysprefs_pid)
    if [ -z "$PID" ]; then skip "System Preferences not running (open it to check the osax)"
    else
        log=$(sudo grep -a "$PID" /var/log/system.log 2>/dev/null | grep "MT2PaneRefresh")
        echo "$log" | grep -q "image loaded into pid" && green "osax injected (pid $PID)" || red "osax NOT injected (watcher/SIMBL race — relaunch System Prefs)"
        echo "$log" | grep -q "swizzled IOBluetoothDevice image" && green "BT-pane MT2 icon: image swizzle installed" || info "icon: swizzle not in yet (open Bluetooth pane)"
        echo "$log" | grep -q "rebound MT2 row icon" && green "BT-pane MT2 icon: row rebound to our art" || info "icon: row not rebound yet (open Bluetooth pane)"
        echo "$log" | grep -q "usb-battery: row painted" && green "USB battery row painted" || info "USB battery: n/a (open Trackpad pane on USB)"
        echo "$log" | grep -q "hid Change-Batteries" && info "Change-Batteries hidden (only logs when it was showing)"
    fi

    echo
    echo "== manual visual checks (a script can't read these) =="
    echo "  BT menu-extra battery: click the Bluetooth menu-bar item -> Magic Trackpad 2 -> Battery: NN%"
    echo "     (expected to match [1]/[3] with NO injection — it reads the same node; requires MT2 on BT)"
    echo "  Trackpad pane (BT): battery row shows NN%, NO 'Change Batteries...' button"
    echo "  Trackpad pane (USB): battery row shows 'NN% (charging)'"
    echo "  Bluetooth pane: Magic Trackpad 2 row shows the trackpad icon (not the generic BT logo)"
    echo "  Transitions (use 'watch' mode below): USB->BT hands the battery row back to Apple (no"
    echo "     '(charging)' residue); unplug-while-on-USB -> NoTrackpad (no stale row); no only-open flash."
    echo
    echo "== $PASS pass / $FAIL fail / $SKIP skip =="
    [ "$FAIL" -eq 0 ]
}

# ---- live watch (capture transitions while you cycle the cable) --------------------------------
watch_mode() {
    echo "== watch: sampling transport + battery node every 1.5s. Cycle USB<->BT now; Ctrl-C to stop. =="
    last=""
    while true; do
        T=$(transport)
        bp=$(ioreg -r -c BNBTrackpadDevice -w0 2>/dev/null | grep '"BatteryPercent"' | grep -oE '[0-9]+$')
        cur="$T batt=${bp:-none}"
        if [ "$cur" != "$last" ]; then
            printf '%s  %s\n' "$(date '+%H:%M:%S')" "$cur"
            last="$cur"
        fi
        # busy-wait ~1.5s without foreground sleep dependencies
        i=0; while [ $i -lt 45000 ]; do i=$((i+1)); done
    done
}

case "${1:-snapshot}" in
    watch) watch_mode ;;
    *)     snapshot ;;
esac
