#!/bin/sh
# Fetch + cache + checksum-verify MacOSX10.9.sdk, used ONLY to cross-build the
# kext from a modern host (the native 10.9 dev box uses its own system SDK).
# Prints the SDK root path on stdout. Apple SDK bytes are never committed to the
# repo -- this is a build-time fetch, same footprint as dimmit's dependency fetch.
set -eu
CACHE="${MT2_SDK_CACHE:-${TMPDIR:-/tmp}/mt2-sdk-cache}"
URL="https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.9.sdk.tar.xz"
SHA="fcf88ce8ff0dd3248b97f4eb81c7909f2cc786725de277f4d05a2b935cc49de0"
SDK="$CACHE/MacOSX10.9.sdk"
mkdir -p "$CACHE"
if [ ! -d "$SDK" ]; then
  TARBALL="$CACHE/MacOSX10.9.sdk.tar.xz"
  [ -f "$TARBALL" ] || curl -sL --fail -o "$TARBALL" "$URL"
  echo "$SHA  $TARBALL" | shasum -a 256 -c - >&2
  tar xf "$TARBALL" -C "$CACHE"
fi
[ -f "$SDK/System/Library/Frameworks/Kernel.framework/Headers/IOKit/IOService.h" ] \
  || { echo "SDK missing kernel headers: $SDK" >&2; exit 1; }
echo "$SDK"
