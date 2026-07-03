#!/bin/sh
# Round-trip regression test for the native EdDSA signer (mt2_sign_update / mt2_generate_keys).
# Proves, with no network and no real secret:
#   - a generated key signs, and the signature self-verifies (so the framework would accept it),
#   - the exact Sparkle enclosure format is emitted,
#   - signing is deterministic (RFC 8032 -> byte-identical, so it matches the official sign_update),
#   - the signature is bound to the file content,
#   - a corrupt key is rejected (the signer's self-check fails loudly rather than emitting garbage).
# Args: $1 = mt2_sign_update binary, $2 = mt2_generate_keys binary (passed by ctest as TARGET_FILEs).
set -eu

SIGN="$1"; GEN="$2"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/mt2signer.XXXXXX")   # template form -> portable (BSD + GNU mktemp)
trap 'rm -rf "$TMP"' EXIT

# 1. Generate a throwaway keypair; grab the 96-byte private blob line.
KEY=$("$GEN" | awk '/Private key/{getline; gsub(/ /,""); print}')
[ -n "$KEY" ] || { echo "FAIL: generate_keys produced no private key"; exit 1; }

# 2. Sign a fixed 24-byte vector -- must exit 0 (self-check verified the sig against the key's public
#    half) and emit exactly the appcast enclosure attributes.
printf 'mt2 signer ctest vector\n' > "$TMP/a.bin"
OUT=$("$SIGN" -s "$KEY" "$TMP/a.bin")
echo "$OUT" | grep -qE '^sparkle:edSignature="[A-Za-z0-9+/]+=*" length="24"$' \
    || { echo "FAIL: unexpected signer output: [$OUT]"; exit 1; }

# 3. Deterministic: signing the same bytes again is byte-identical.
OUT2=$("$SIGN" -s "$KEY" "$TMP/a.bin")
[ "$OUT" = "$OUT2" ] || { echo "FAIL: signature is not deterministic"; exit 1; }

# 4. Content-bound: a different file yields a different signature.
printf 'mt2 signer ctest vector!\n' > "$TMP/b.bin"
OUTB=$("$SIGN" -s "$KEY" "$TMP/b.bin")
[ "$OUT" != "$OUTB" ] || { echo "FAIL: signature did not change with content"; exit 1; }

# 5. Corrupt key (one flipped base64 char -> same length, wrong key) must be rejected: the public half
#    no longer matches the private scalar, so the self-check fails and the tool exits non-zero.
FIRST=$(printf '%s' "$KEY" | cut -c1)
REST=$(printf '%s' "$KEY" | cut -c2-)
if [ "$FIRST" = "A" ]; then BADKEY="B$REST"; else BADKEY="A$REST"; fi
if "$SIGN" -s "$BADKEY" "$TMP/a.bin" >/dev/null 2>&1; then
    echo "FAIL: a corrupt key was accepted"; exit 1
fi

echo "PASS: signer round-trip (generate, sign, self-verify, deterministic, content-bound, reject-bad-key)"
