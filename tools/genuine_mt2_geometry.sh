#!/bin/sh
# Capture a GENUINE Magic Trackpad 2's multitouch geometry on modern macOS, to diff against what our
# 10.9 kext seeds. Our seeds (the suspects): Sensor Region = ALL ZEROS, Sensor Rows 13 / Columns 16,
# Surface Width 16000 / Height 11300. We've proven (on 10.9) the recognizer's position-normalization
# rectangle is ~HALF the physical pad and offset up in Y (active ~80x64mm vs physical 160x115mm), which
# freezes the perpendicular axis in the L/R and top edge bands. We need the REAL device's geometry to
# see which input sizes/places that rectangle.
#
# Run this on the modern Mac with the genuine MT2 connected (BT or USB). It writes its output to a file
# RIGHT NEXT TO this script (so on the shared NFS it lands back on the 10.9 box automatically).

OUT="$(cd "$(dirname "$0")" && pwd)/genuine-mt2-geometry.out"

{
  echo "=== genuine MT2 geometry capture ==="
  echo "date : $(date)"
  echo "macOS: $(sw_vers 2>/dev/null | tr '\n' ' ')"
  echo "model: $(sysctl -n hw.model 2>/dev/null)"
  echo

  echo "=== A) grepped geometry properties (whole ioreg) — the quick diff ==="
  ioreg -lw0 2>/dev/null | grep -iE "Sensor Surface|Sensor Region|Sensor Rows|Sensor Column|Family ID|parser-type|parser-options|Dimensions|Built|Surface Descriptor|Region Param|Region Descriptor|Multitouch ID|Transport|bcdVersion"

  echo
  echo "=== B) full AppleMultitouchDevice node(s) (the recognizer's device — all properties) ==="
  ioreg -lrw0 -c AppleMultitouchDevice 2>/dev/null

  echo
  echo "=== C) full AppleUSBMultitouchDriver node(s) (if cabled over USB) ==="
  ioreg -lrw0 -c AppleUSBMultitouchDriver 2>/dev/null

  echo
  echo "=== D) full BNBTrackpadDevice node(s) (if over Bluetooth; class may differ on modern macOS) ==="
  ioreg -lrw0 -c BNBTrackpadDevice 2>/dev/null

  echo
  echo "=== E) the tree around anything Multitouch/Trackpad (orientation) ==="
  ioreg -lw0 2>/dev/null | grep -iE "Multitouch|Trackpad" | head -60

  echo
  echo "=== done ==="
} > "$OUT" 2>&1

echo "wrote: $OUT"
echo "(bring this back / it should already be visible over the shared NFS)"
