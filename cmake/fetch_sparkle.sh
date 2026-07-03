#!/bin/sh
# Fetch + cache + checksum-verify the PREBUILT Sparkle 1.27.3 framework (the last Sparkle that runs
# on 10.9; LC_VERSION_MIN_MACOSX 10.9 verified on-device). Thins to the x86_64 slice (the release
# framework is fat x86_64+arm64; 10.9 is Intel-only). Prints the Sparkle.framework path on stdout.
# Sparkle bytes are never committed -- build-time fetch, same pattern as fetch_10_9_sdk.sh.
set -eu
CACHE="${MT2_SPARKLE_CACHE:-${TMPDIR:-/tmp}/mt2-sparkle-cache}"
URL="https://github.com/sparkle-project/Sparkle/releases/download/1.27.3/Sparkle-1.27.3.tar.xz"
SHA="b4c70198aba86a65dc04550fbd0a97243a9ba3b98d73d138c877347f27920952"
FW="$CACHE/Sparkle.framework"
mkdir -p "$CACHE"
if [ ! -d "$FW" ]; then
  TARBALL="$CACHE/Sparkle-1.27.3.tar.xz"
  [ -f "$TARBALL" ] || curl -sL --fail -o "$TARBALL" "$URL"
  echo "$SHA  $TARBALL" | shasum -a 256 -c - >&2
  tar xf "$TARBALL" -C "$CACHE" Sparkle.framework
  # Thin the framework binary + Autoupdate helper to x86_64 (10.9 is Intel-only).
  for bin in "$FW/Versions/A/Sparkle" "$FW/Versions/A/Resources/Autoupdate.app/Contents/MacOS/Autoupdate"; do
    [ -f "$bin" ] && lipo "$bin" -verify_arch x86_64 2>/dev/null && lipo -thin x86_64 "$bin" -output "$bin.x" && mv "$bin.x" "$bin" || true
  done
fi
[ -f "$FW/Versions/A/Sparkle" ] || { echo "Sparkle fetch failed: $FW" >&2; exit 1; }
echo "$FW"
