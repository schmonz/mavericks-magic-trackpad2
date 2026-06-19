#!/bin/sh
# run_tap_trace.sh - orchestrate the round-5 tap-blocker capture.
#
# Attaches trace_tap.d to hidd (the recognizer host), injects a few synthetic taps via
# synth_tap, then stops the trace and prints the summary. Decides H1 (chord-motion) vs
# H2 (wrong-gate) — see tools/trace_tap.d.
#
# Run as root, with the branch kext loaded and the console session unlocked:
#   sudo tools/run_tap_trace.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DSCRIPT=${1:-$HERE/trace_tap.d}
LOG=${LOG:-/tmp/tap_trace.log}
HIDD=$(pgrep -x hidd | head -1)
[ -n "$HIDD" ] || { echo "hidd not running?" >&2; exit 1; }
echo "hidd pid=$HIDD  log=$LOG"

: > "$LOG"
# trace_tap.d self-terminates after ~9s and flushes its END summary.
dtrace -q -s "$DSCRIPT" -p "$HIDD" >"$LOG" 2>&1 &
DT=$!
# wait for attach (banner appears once instrumentation is live)
i=0; while [ $i -lt 60 ]; do grep -q 'inject taps now\|gate trace' "$LOG" 2>/dev/null && break; i=$((i+1)); sleep 0.1; done
echo "dtrace attached (pid $DT); injecting taps..."

# a handful of taps, center, ~60ms each, spaced out — all within the ~9s window
n=0
while [ $n -lt 4 ]; do
  n=$((n+1))
  echo "--- tap $n ---"
  "$HERE/synth_tap" ${TAPARGS:-5 12 0 0} 2>&1 | sed 's/^/    /'
  sleep 0.5
done

echo "waiting for trace to self-stop + flush..."
wait "$DT" 2>/dev/null || true
echo
echo "================= TRACE OUTPUT ================="
cat "$LOG"
