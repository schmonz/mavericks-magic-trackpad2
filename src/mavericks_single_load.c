#include "mavericks_single_load.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Process-global marker, KEYED BY PID. An environment variable is visible to every Mach-O image in the
 * process (unlike a per-image static), so the osax and SIMBL payloads observe each other's claim. But
 * env vars are INHERITED by child processes — the updater helper we launch, and anything further down
 * that launch chain, carries a copy. A presence-only flag ("is it set?") therefore made a NEW
 * payload-hosting System Preferences that happened to inherit the marker believe the claim was already
 * taken and stay fully inert — no swizzles, no About tab (observed on-device: whole fresh instances
 * logging "lost the single-load claim"; the About tab intermittently vanished across reopens).
 *
 * Fix: store OUR pid and claim unless the marker already equals it. An inherited marker always carries
 * a DIFFERENT pid (a child's pid != its parent's), so a foreign value is treated as stale and we claim
 * fresh; only a value equal to our own pid means another image IN THIS SAME PROCESS already claimed, so
 * that copy stays inert. Constructors run on the main thread at image-load before any concurrency, so
 * the non-atomic getenv/setenv pair is race-free here.
 *
 * STABILITY: keep this marker name FIXED across versions and the upcoming product rename. If it ever
 * changed, an old-named and a new-named payload loaded into the SAME process would not see each other's
 * claim and both would activate (double-swizzle during a cross-version overlap). The name is internal,
 * so a rename gains nothing — leave it as-is. */
int mavericks_claim_single_load(void) {
    char me[16];
    snprintf(me, sizeof me, "%d", (int)getpid());
    const char *v = getenv(MT2_SINGLE_LOAD_ENV);
    if (v && strcmp(v, me) == 0) return 0;   /* another image in THIS process already claimed */
    /* Fail CLOSED: if we cannot record the claim (setenv OOM), do NOT claim — zero active payloads
     * is safer than two. */
    if (setenv(MT2_SINGLE_LOAD_ENV, me, 1) != 0) return 0;
    return 1;
}
