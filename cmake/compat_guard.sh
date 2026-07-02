#!/bin/sh
# 10.9 userland compat guard.
#
# Fail the build if any SHIPPED userland binary references a post-10.9 symbol as an
# undefined import -- on a real 10.9 machine dyld aborts at launch on a non-weak
# missing symbol, and even a weak one means we're calling an API that isn't there.
# Cross-building against the modern SDK makes this a live regression risk, so this
# runs in CI after every build.
#
# Audited 2026-07-02: the tree currently has ZERO such references (the dispatch_*
# symbols present are libdispatch, shipped since 10.6). This guard keeps it that way.
#
# Usage: compat_guard.sh <build-dir>
set -eu
BUILD=$1

# APIs introduced AFTER OS X 10.9 that must never appear as undefined imports.
# (name -> first OS). Extend as new call sites are added.
POST_10_9='_clock_gettime|_clock_gettime_nsec_np|_IORegistryEntryCopyPath|_IOMainPort|_kIOMainPortDefault|_os_unfair_lock_lock|_os_unfair_lock_unlock|_os_log|_os_log_create|_os_log_impl'

# The binaries the .pkg actually ships (mirror cmake/mt2_pkg.cmake).
SHIPPED="
$BUILD/sbin/mt2_reenumerate
$BUILD/sbin/mt2_set_btname
$BUILD/sbin/mt2_pane_watch
$BUILD/MT2PaneRefresh.osax/Contents/MacOS/MT2PaneRefresh
$BUILD/MT2PaneRefresh.bundle/Contents/MacOS/MT2PaneRefresh
"

fail=0
checked=0
for b in $SHIPPED; do
  [ -f "$b" ] || { echo "compat guard: MISSING shipped binary $b" >&2; fail=1; continue; }
  checked=$((checked + 1))
  # Undefined symbols only; match the denylist as whole symbol names.
  leak=$(nm -u "$b" 2>/dev/null | awk '{print $NF}' | grep -xE "($POST_10_9)" || true)
  if [ -n "$leak" ]; then
    echo "compat guard: post-10.9 symbol(s) in $b:" >&2
    printf '  %s\n' $leak >&2
    fail=1
  fi
done

# Fail closed: if we somehow measured nothing, that's a broken build, not a pass.
[ "$checked" -gt 0 ] || { echo "compat guard: no shipped binaries found under $BUILD (fail-closed)" >&2; exit 2; }

[ "$fail" = 0 ] && echo "compat guard: $checked shipped binaries clean (no post-10.9 imports)" || exit 1
