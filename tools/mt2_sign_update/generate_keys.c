/*
 * mt2_generate_keys — a native, 10.9-buildable Sparkle EdDSA keypair generator.
 *
 * Prints the public key (for Info.plist SUPublicEDKey) and the 96-byte private blob
 * (private[64] || public[32], base64) that mt2_sign_update / the official sign_update take via -s.
 * Uses the same orlp/ed25519 library, so the keys are interchangeable with Sparkle's own generate_keys.
 *
 * Generating a key is NOT a habitual action (do it once, store the private blob as a CI secret) -- this
 * exists so the 10.9 box is self-sufficient and never needs a modern Swift toolchain. The seed comes
 * from /dev/urandom (orlp/ed25519's ed25519_create_seed).
 */
#include <stdio.h>
#include <string.h>
#include "ed25519.h"
#include "mt2_b64.h"

int main(void) {
    unsigned char seed[32], pub[32], priv[64], blob[96];
    if (ed25519_create_seed(seed) != 0) {
        fprintf(stderr, "failed to read a random seed from /dev/urandom\n");
        return 1;
    }
    ed25519_create_keypair(pub, priv, seed);
    memcpy(blob, priv, 64);
    memcpy(blob + 64, pub, 32);

    char pubb64[64], blobb64[192];
    mt2_b64_encode(pub, 32, pubb64);
    mt2_b64_encode(blob, 96, blobb64);

    printf("Public key  (Info.plist SUPublicEDKey):\n  %s\n\n", pubb64);
    printf("Private key (keep SECRET; store as SPARKLE_PRIVATE_KEY, pass to sign_update -s):\n  %s\n", blobb64);
    return 0;
}
