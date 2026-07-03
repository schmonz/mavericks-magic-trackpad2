/*
 * mt2_sign_update — a native, 10.9-buildable Sparkle EdDSA signer.
 *
 *     mt2_sign_update -s <base64 private key> <file>
 *
 * Prints the appcast enclosure attributes Sparkle expects, exactly like the official sign_update:
 *
 *     sparkle:edSignature="<base64 signature>" length="<file size in bytes>"
 *
 * WHY this exists: the official sign_update is a modern Swift binary that cannot run on 10.9. This
 * tool uses the SAME ed25519 library Sparkle vendors (orlp/ed25519, fetched by cmake/fetch_ed25519.sh),
 * so the 64-byte signature is byte-identical to the official tool (ed25519 is deterministic in the
 * key + message) and the framework's SUPublicEDKey verification accepts it. Signing is habitual, so we
 * want it available locally as well as in CI.
 *
 * KEY FORMAT: Sparkle's private key is the 96-byte blob private[64] || public[32] (128 base64 chars;
 * the official tool's own help: "The key's length is 128 that includes private and public key").
 * private[64] is orlp/ed25519's EXPANDED key (clamped SHA-512 of the seed), which is what ed25519_sign
 * takes directly.
 *
 * SELF-CHECK: after signing we ed25519_verify the signature against the public half of the blob.
 * ed25519_verify passes iff that public key equals the private scalar's point -- so a pass means the
 * signature verifies against the SUPublicEDKey the framework will use, and a mismatched key or a wrong
 * private/public split fails loudly instead of emitting a signature the updater would reject.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ed25519.h"
#include "mt2_b64.h"

int main(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[1], "-s") != 0) {
        fprintf(stderr, "usage: mt2_sign_update -s <base64 private key> <file>\n");
        return 2;
    }
    const char *keyb64 = argv[2];
    const char *path   = argv[3];

    unsigned char key[128];
    long klen = mt2_b64_decode(keyb64, key);
    if (klen != 96) {
        fprintf(stderr, "bad private key: decoded %ld bytes, expected 96 (private[64] || public[32])\n", klen);
        return 1;
    }
    const unsigned char *priv = key;        /* [0:64]  expanded private key */
    const unsigned char *pub  = key + 64;   /* [64:96] public key           */

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fprintf(stderr, "seek failed\n"); return 1; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); fprintf(stderr, "tell/seek failed\n"); return 1; }
    unsigned char *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); fprintf(stderr, "out of memory\n"); return 1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); fprintf(stderr, "read error\n"); return 1; }
    fclose(f);

    unsigned char sig[64];
    ed25519_sign(sig, buf, (size_t)n, pub, priv);

    if (!ed25519_verify(sig, buf, (size_t)n, pub)) {
        free(buf);
        fprintf(stderr, "signature self-check failed -- bad key, or the blob is not private[64]||public[32]\n");
        return 1;
    }
    free(buf);

    char sigb64[128];
    mt2_b64_encode(sig, 64, sigb64);
    printf("sparkle:edSignature=\"%s\" length=\"%ld\"\n", sigb64, n);
    return 0;
}
