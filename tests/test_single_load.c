#include "../src/mt2_single_load.h"
#include "test.h"
#include <stdlib.h>

/* First-loader-wins guard: the FIRST call in a process claims (returns 1); every subsequent call
 * bails (returns 0). This is what stops the osax + SIMBL payloads double-swizzling when both load
 * into one System Preferences process. Simulated here by calling repeatedly in one process — the
 * env marker spans repeated calls the same way it spans separately-loaded images. A regression here
 * silently re-enables double injection (double icon swizzle / duplicated pane controls). */
static void run_tests(void) {
    /* Deterministic start: clear the marker regardless of the caller's inherited environment. */
    unsetenv(MT2_SINGLE_LOAD_ENV);

    /* First loader wins. */
    CHECK_EQ(mt2_claim_single_load(), 1);

    /* Every subsequent copy bails. */
    CHECK_EQ(mt2_claim_single_load(), 0);
    CHECK_EQ(mt2_claim_single_load(), 0);

    /* Simulate a fresh process by clearing the marker: the next loader can then claim again. */
    unsetenv(MT2_SINGLE_LOAD_ENV);
    CHECK_EQ(mt2_claim_single_load(), 1);
}

TEST_MAIN()
