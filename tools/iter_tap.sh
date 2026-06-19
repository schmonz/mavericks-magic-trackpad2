#!/bin/sh
# iter_tap.sh - hands-free tap-to-click iteration loop. Rebuilds synth_tap (so any
# src/ liftoff change is included), traces hidd's tap->click decision chain, injects N
# synthetic taps, and prints the summary counts. No real finger needed.
#
# Read the summary like this:
#   selectTapChord     == taps RECOGNIZED   (want N)
#   handleChordLiftoff == liftoffs           (want N, i.e. 1/tap; 2/tap == the double-liftoff bug)
#   queueButtonClick   == clicks committed   (want N)
#
#   sudo tools/iter_tap.sh [ntaps]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
N=${1:-6}
GAP=${TAPGAP:-0.7}
cc -I "$ROOT/src" -o "$HERE/synth_tap" "$HERE/synth_tap.c" \
   "$ROOT/src/mt2_session.c" "$ROOT/src/mt2_lifecycle.c" \
   "$ROOT/src/mt2_pipeline.c" "$ROOT/src/mt1_encode.c" \
   -framework IOKit -framework CoreFoundation 2>/dev/null
HIDD=$(pgrep -x hidd | head -1)
LOG=${LOG:-/tmp/iter_tap.log}
dtrace -q -s "$HERE/trace_tapclick.d" -p "$HIDD" >"$LOG" 2>&1 &
DT=$!
sleep 2
i=0; while [ $i -lt "$N" ]; do i=$((i+1)); X=$(( (i * 500) - 1500 )); "$HERE/synth_tap" 5 12 "$X" 0 >/dev/null 2>&1; sleep "$GAP"; done
sleep 1                       # drain trailing liftoff events
kill -INT "$DT" 2>/dev/null || true   # dtrace flushes its END summary on SIGINT
wait "$DT" 2>/dev/null || true
echo "==== $N synthetic taps ===="
sed -n '/== summary/,$p' "$LOG"
