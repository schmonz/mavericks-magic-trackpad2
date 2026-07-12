#!/bin/sh
# Fetch + cache + checksum-verify orlp/ed25519 -- the SAME ed25519 library Sparkle vendors for its
# EdDSA signing/verification. Using the same library means our native signer produces byte-identical
# signatures (ed25519 is deterministic) that the Sparkle framework's SUPublicEDKey check accepts, and
# it builds on the native 10.9 toolchain (plain C, no Swift). Prints the src/ dir (the *.c + *.h) on
# stdout. Bytes are never committed -- build-time fetch, same pattern as fetch_sparkle.sh.
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

CACHE="${MT2_ED25519_CACHE:-$HOME/Library/Caches/mt2-ed25519}"
COMMIT="b1f19fab4aebe607805620d25a5e42566ce46a0e"
URL="https://github.com/orlp/ed25519/archive/$COMMIT.tar.gz"
SHA="aedb26c46d3dc3b721ab37c5248d5c923142e4d56009a9605c470383f32ce77a"
SRC="$CACHE/ed25519-$COMMIT/src"
if [ ! -f "$SRC/sign.c" ]; then
  mav_fetch_pinned "$URL" "$SHA" "$CACHE" "ed25519-$COMMIT.tar.gz"
fi
[ -f "$SRC/sign.c" ] || { echo "ed25519 fetch failed: $SRC" >&2; exit 1; }
echo "$SRC"
