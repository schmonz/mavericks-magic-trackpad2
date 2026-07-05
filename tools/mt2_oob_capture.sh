#!/bin/sh
# mt2_oob_capture.sh — turnkey capture of the DEVICE-SIDE link-key write during USB
# out-of-band ("MagicPairing" / HID-Emulation) pairing on a modern macOS, logged to a file.
#
# Run it on the modern Mac (e.g. taavibookair / Sequoia); it self-elevates with sudo,
# attaches dtrace to bluetoothd via mt2_oob_capture.d, and tees output to a log file.
#
#   sh /Users/schmonz/Documents/code/trees/mavericks-magic-trackpad2/tools/mt2_oob_capture.sh
#   # (optional) pass a log path:  sh …/mt2_oob_capture.sh /tmp/mycapture.txt
#
# Then: make sure the MT2 is NOT paired to this Mac (Forget it in System Settings >
# Bluetooth if listed) and PLUG IT IN over USB. Ctrl-C when the SET_REPORT blocks appear.
set -u

SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
DSCRIPT=$(dirname "$SELF")/mt2_oob_capture.d
OUT="${1:-/tmp/mt2_oob_capture-$(date +%Y%m%d-%H%M%S).txt}"

# Self-elevate (single password prompt). Use `sudo sh` so no exec bit is needed over NFS.
if [ "$(id -u)" -ne 0 ]; then
    echo "Elevating with sudo (you'll be asked for your password once)…"
    exec sudo sh "$SELF" "$OUT"
fi

[ -f "$DSCRIPT" ] || { echo "ERROR: dtrace script not found: $DSCRIPT" >&2; exit 1; }

PID=$(pgrep -x bluetoothd | head -1)
[ -n "${PID:-}" ] || { echo "ERROR: bluetoothd is not running" >&2; exit 1; }

# Self-contained log header.
{
    echo "# mt2 OOB capture — $(date)"
    echo "# host=$(hostname)  bluetoothd pid=$PID"
    echo "# dtrace script=$DSCRIPT"
    echo "# reportType legend: 0=Input 1=Output 2=Feature"
    echo "# ------------------------------------------------------------------------"
} > "$OUT"

cat <<EOF
mt2 OOB capture
  bluetoothd pid : $PID
  logging to     : $OUT

>>> Make sure the Magic Trackpad 2 is NOT paired to this Mac (Forget it under
    System Settings > Bluetooth if it's listed), then PLUG IT IN over USB.
    The out-of-band pairing writes the link key to the device; the SET_REPORT(s)
    print below and are saved to the log. Press Ctrl-C when done.

If dtrace errors with "cannot control ... restricted executable", SIP is blocking
it: reboot to Recovery -> \`csrutil disable\` (and if still blocked, add boot-arg
\`amfi_get_out_of_my_way=1\`), then re-run this script.
--------------------------------------------------------------------------------
EOF

# Attach to bluetoothd; tee live output into the log (appending after the header).
dtrace -s "$DSCRIPT" -p "$PID" 2>&1 | tee -a "$OUT"

echo "--------------------------------------------------------------------------------"
echo "Saved capture to: $OUT"
echo "Send me that file (or paste its contents) and I'll decode the report."
