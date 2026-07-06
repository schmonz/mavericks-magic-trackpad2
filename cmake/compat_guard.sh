#!/bin/sh
# 10.9 userland compat guard.
#
# Fail the build if any SHIPPED userland binary would misbehave on a real 10.9 machine.
# Three per-binary invariants, checked for every shipped userland Mach-O:
#   (1) no post-10.9 symbol as an undefined import -- on a real 10.9 machine dyld aborts
#       at launch on a non-weak missing symbol, and even a weak one means we're calling an
#       API that isn't there. Cross-building against the modern SDK makes this a live risk.
#   (2) architecture is exactly x86_64 -- 10.9 is Intel-only; an arm64 (or fat) slice can
#       never run there.
#   (3) LC_VERSION_MIN_MACOSX minimum OS is 10.9 -- a higher deployment target lets the
#       linker resolve/emit newer-OS behavior and marks the binary as requiring a newer OS.
#
# Audited 2026-07-02: the tree currently has ZERO post-10.9 references (the dispatch_*
# symbols present are libdispatch, shipped since 10.6). This guard keeps it that way.
#
# Userland only: the kext is NOT checked here (kext loadability is a separate task).
#
# Usage: compat_guard.sh <build-dir>
set -eu
BUILD=$1

# APIs introduced AFTER OS X 10.9 that must never appear as undefined imports.
# (name -> first OS). Extend as new call sites are added.
POST_10_9='_clock_gettime|_clock_gettime_nsec_np|_IORegistryEntryCopyPath|_IOMainPort|_kIOMainPortDefault|_os_unfair_lock_lock|_os_unfair_lock_unlock|_os_log|_os_log_create|_os_log_impl'

fail=0
checked=0

# check_binary <path>
# Runs all three invariants against one Mach-O. Sets $fail=1 on any violation and
# names the binary + the violation + the fix on stderr. Increments $checked.
check_binary() {
  b=$1
  [ -f "$b" ] || { echo "compat guard: MISSING shipped binary $b" >&2; fail=1; return; }
  checked=$((checked + 1))

  # (1) post-10.9 undefined imports. Match the denylist as whole symbol names.
  leak=$(nm -u "$b" 2>/dev/null | awk '{print $NF}' | grep -xE "($POST_10_9)" || true)
  if [ -n "$leak" ]; then
    echo "compat guard: post-10.9 symbol(s) in $b:" >&2
    printf '  %s\n' $leak >&2
    echo "  fix: drop the post-10.9 API / use a 10.9-available equivalent." >&2
    fail=1
  fi

  # (2) architecture must be exactly x86_64 (10.9 is Intel-only).
  #     `lipo -archs` doesn't exist on the 10.9 toolchain, so parse `lipo -info`:
  #       non-fat -> "...is architecture: x86_64"; fat -> "...are: x86_64 arm64".
  #     Stripping through the last ": " yields the space-separated arch list either way.
  archs=$(lipo -info "$b" 2>/dev/null | sed 's/.*: //' || true)
  if [ "$archs" != "x86_64" ]; then
    echo "compat guard: $b arch is '$archs', expected exactly 'x86_64' (10.9 is Intel-only)." >&2
    echo "  fix: build for x86_64 only (CMAKE_OSX_ARCHITECTURES=x86_64)." >&2
    fail=1
  fi

  # (3) minimum OS must be 10.9 -- from EITHER LC_VERSION_MIN_MACOSX ('version' field, what the 10.9-era
  #     toolchain emits on the native x86 host) OR LC_BUILD_VERSION ('minos' field, what a modern arm64
  #     toolchain on macOS 26 emits when it cross-builds for 10.9). Accept both so this guard runs
  #     EQUALLY on the 10.9 x86 host and the macOS 26 arm64 host.
  minos=$(otool -l "$b" 2>/dev/null | awk '
    /LC_VERSION_MIN_MACOSX/ { inv = 1; next }
    inv && $1 == "version" { print $2; exit }
    /LC_BUILD_VERSION/ { inb = 1; next }
    inb && $1 == "minos" { print $2; exit }
  ')
  if [ -z "$minos" ]; then
    echo "compat guard: $b has no LC_VERSION_MIN_MACOSX/LC_BUILD_VERSION min-OS (cannot prove 10.9)." >&2
    echo "  fix: set the deployment target (CMAKE_OSX_DEPLOYMENT_TARGET=10.9)." >&2
    fail=1
  elif [ "$minos" != "10.9" ]; then
    echo "compat guard: $b min-OS is '$minos', expected '10.9'." >&2
    echo "  fix: set CMAKE_OSX_DEPLOYMENT_TARGET=10.9." >&2
    fail=1
  fi
}

# The binaries the .pkg actually ships (mirror cmake/mt2_pkg.cmake).
SHIPPED="
$BUILD/sbin/mt2_reenumerate
$BUILD/sbin/mt2_pane_watch
$BUILD/MT2PaneRefresh.osax/Contents/MacOS/MT2PaneRefresh
$BUILD/MT2PaneRefresh.bundle/Contents/MacOS/MT2PaneRefresh
"

for b in $SHIPPED; do
  check_binary "$b"
done

# Updater binary: only present in Sparkle-enabled builds; skip silently if absent.
UPD_BIN="$BUILD/MavericksTrackpad2Updater.app/Contents/MacOS/MavericksTrackpad2Updater"
if [ -f "$UPD_BIN" ]; then
  check_binary "$UPD_BIN"
fi

# Fail closed: if we somehow measured nothing, that's a broken build, not a pass.
[ "$checked" -gt 0 ] || { echo "compat guard: no shipped binaries found under $BUILD (fail-closed)" >&2; exit 2; }

[ "$fail" = 0 ] && echo "compat guard: $checked shipped binaries clean (x86_64, min-OS 10.9, no post-10.9 imports)" || exit 1
