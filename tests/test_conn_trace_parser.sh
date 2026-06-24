#!/bin/sh
# Asserts `tools/re conn-trace` renders the right per-connection verdict for known fixtures.
set -e
here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
ct="$root/tools/re"

out_ok=$("$ct" conn-trace < "$root/tests/fixtures/conn-trace-steady.log")
echo "$out_ok" | grep -q 'VERDICT: STEADY conn=1' || { echo "FAIL: steady fixture not STEADY"; echo "$out_ok"; exit 1; }

out_bad=$("$ct" conn-trace < "$root/tests/fixtures/conn-trace-fail.log")
echo "$out_bad" | grep -q 'VERDICT: FAIL conn=1 .*stalled at INTERPOSED' || { echo "FAIL: fail fixture not FAIL@INTERPOSED"; echo "$out_bad"; exit 1; }

echo OK
