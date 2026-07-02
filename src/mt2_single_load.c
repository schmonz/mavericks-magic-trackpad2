#include "mt2_single_load.h"
#include <stdlib.h>

/* Process-global marker. An environment variable is visible to every Mach-O image in the process
 * (unlike a per-image static), so the osax and SIMBL payloads observe each other's claim. The value
 * is irrelevant — presence is the flag. A copy leaked into child processes we later launch (e.g. the
 * updater helper) is harmless: only our payload code reads it, and it is not loaded into those
 * children. Runs on the main thread at image-load (constructor) before any concurrency, so the
 * non-atomic getenv/setenv pair is safe here.
 *
 * STABILITY: keep this marker name FIXED across versions and the upcoming product rename. If it ever
 * changed, an old-named and a new-named payload loaded into the SAME process would not see each
 * other's claim and both would activate (double-swizzle during a cross-version overlap). The name is
 * internal, so a rename gains nothing — leave it as-is. */
int mt2_claim_single_load(void) {
    if (getenv(MT2_SINGLE_LOAD_ENV)) return 0;   /* another copy already claimed it */
    /* Fail CLOSED: if we cannot record the claim (setenv OOM), do NOT claim — zero active payloads
     * is safer than two. */
    if (setenv(MT2_SINGLE_LOAD_ENV, "1", 1) != 0) return 0;
    return 1;
}
