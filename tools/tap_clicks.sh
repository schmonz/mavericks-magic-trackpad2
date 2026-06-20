#!/bin/sh
# tap_clicks.sh - hands-free CGEvent oracle for tap-to-click. Rebuilds synth_tap (so any src/
# change is included), streams N taps through ONE held-open connection, and classifies the
# resulting CGEvent clicks into CLEAN vs PHANTOM.
#
#   PHANTOM = a LeftDown arriving < 25ms after the previous event (the ~6ms double-click bug).
#   CLEAN   = a LeftDown well-separated from the prior click (a real single tap-click).
#
# Each run prints one line:  taps=N  downs=D  clean=C  phantom=P
#
#   sudo tools/tap_clicks.sh [ntaps] [gap_ms]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/.." && pwd)
N=${1:-10}; GAP=${2:-500}; DF=${3:-5}; FMS=${4:-12}
cc -I "$ROOT/src" -o "$HERE/synth_tap" "$HERE/synth_tap.c" \
   "$ROOT/src/mt2_session.c" "$ROOT/src/mt2_lifecycle.c" \
   "$ROOT/src/mt2_pipeline.c" "$ROOT/src/mt1_encode.c" \
   -framework IOKit -framework CoreFoundation 2>/dev/null
CM=/tmp/tapclicks_cm.log
WINDOW=$(( (N * (GAP + 80)) / 1000 + 6 ))
"$HERE/click_monitor" "$WINDOW" >"$CM" 2>&1 &
CMPID=$!
sleep 1
SYNTH_TAPS="$N" SYNTH_GAP_MS="$GAP" "$HERE/synth_tap" "$DF" "$FMS" 0 0 >/dev/null 2>&1 || true
wait "$CMPID" 2>/dev/null || true
# classify: a LeftDown with dt < 25ms after the prior event is a phantom; else clean.
# The FIRST LeftDown has dt=0 (nothing precedes it) -- it is the first clean click, never a
# phantom, so exclude it from the <25ms test (else it inflates the phantom count by one).
awk '
  /LeftDown/ {
    d=$0; sub(/.*dt=[ ]*\+?/,"",d); sub(/ms.*/,"",d); dt=d+0;
    downs++; if (downs > 1 && dt < 25) phantom++; else clean++;
  }
  END { printf "downs=%d  clean=%d  phantom=%d\n", downs, clean, phantom }
' "$CM" | sed "s/^/taps=$N gap=${GAP}ms  /"
