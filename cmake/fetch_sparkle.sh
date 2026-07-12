#!/bin/sh
# Fetch + cache + checksum-verify the PREBUILT Sparkle 1.27.3 framework (the last Sparkle that runs
# on 10.9; LC_VERSION_MIN_MACOSX 10.9 verified on-device). Thins to the x86_64 slice (the release
# framework is fat x86_64+arm64; 10.9 is Intel-only). Prints the Sparkle.framework path on stdout.
# Sparkle bytes are never committed -- build-time fetch, same pattern as the shared
# fetch_10_9_sdk.sh (mavericks-shared-cmake).
set -eu
# Locate the installed mavericks-shared-cmake scripts (they hold mavericks-fetch.sh).
_msc="${MAVERICKS_SHARED_SCRIPTS:-}"
if [ -z "$_msc" ]; then
  _dir=$(cat "$HOME/.cmake/packages/MavericksSharedCMake/"* 2>/dev/null | head -1)
  [ -n "$_dir" ] && _msc="$_dir/scripts"
fi
[ -n "${_msc:-}" ] && [ -f "$_msc/mavericks-fetch.sh" ] || {
  echo "mavericks-shared-cmake not installed; see README or set MAVERICKS_SHARED_SCRIPTS" >&2
  exit 1
}
. "$_msc/mavericks-fetch.sh"

CACHE="${MT2_SPARKLE_CACHE:-$HOME/Library/Caches/mt2-sparkle}"
URL="https://github.com/sparkle-project/Sparkle/releases/download/1.27.3/Sparkle-1.27.3.tar.xz"
SHA="b4c70198aba86a65dc04550fbd0a97243a9ba3b98d73d138c877347f27920952"
FW="$CACHE/Sparkle.framework"
if [ ! -d "$FW" ]; then
  mav_fetch_pinned "$URL" "$SHA" "$CACHE" "Sparkle-1.27.3.tar.xz" Sparkle.framework
  # Thin the framework binary + Autoupdate helper to x86_64 (10.9 is Intel-only).
  for bin in "$FW/Versions/A/Sparkle" "$FW/Versions/A/Resources/Autoupdate.app/Contents/MacOS/Autoupdate"; do
    [ -f "$bin" ] && lipo "$bin" -verify_arch x86_64 2>/dev/null && lipo -thin x86_64 "$bin" -output "$bin.x" && mv "$bin.x" "$bin" || true
  done
fi
[ -f "$FW/Versions/A/Sparkle" ] || { echo "Sparkle fetch failed: $FW" >&2; exit 1; }
echo "$FW"
