#!/bin/sh
# Fetch + cache + checksum-verify orlp/ed25519 -- the SAME ed25519 library Sparkle vendors for its
# EdDSA signing/verification. Using the same library means our native signer produces byte-identical
# signatures (ed25519 is deterministic) that the Sparkle framework's SUPublicEDKey check accepts, and
# it builds on the native 10.9 toolchain (plain C, no Swift). Prints the src/ dir (the *.c + *.h) on
# stdout. Bytes are never committed -- build-time fetch, same pattern as fetch_sparkle.sh.
set -eu
CACHE="${MT2_ED25519_CACHE:-${TMPDIR:-/tmp}/mt2-ed25519-cache}"
COMMIT="b1f19fab4aebe607805620d25a5e42566ce46a0e"
URL="https://github.com/orlp/ed25519/archive/$COMMIT.tar.gz"
SHA="aedb26c46d3dc3b721ab37c5248d5c923142e4d56009a9605c470383f32ce77a"
SRC="$CACHE/ed25519-$COMMIT/src"
mkdir -p "$CACHE"
if [ ! -f "$SRC/sign.c" ]; then
  TARBALL="$CACHE/ed25519-$COMMIT.tar.gz"
  [ -f "$TARBALL" ] || curl -sL --fail -o "$TARBALL" "$URL"
  echo "$SHA  $TARBALL" | shasum -a 256 -c - >&2
  tar xzf "$TARBALL" -C "$CACHE"
fi
[ -f "$SRC/sign.c" ] || { echo "ed25519 fetch failed: $SRC" >&2; exit 1; }
echo "$SRC"
