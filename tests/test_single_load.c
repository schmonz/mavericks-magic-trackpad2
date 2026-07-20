#include "../src/mavericks_single_load.h"
#include "test.h"
#include <stdlib.h>

/* First-loader-wins guard: the FIRST call in a process claims (returns 1); every subsequent call
 * bails (returns 0). This is what stops the osax + SIMBL payloads double-swizzling when both load
 * into one System Preferences process. Simulated here by calling repeatedly in one process — the
 * env marker spans repeated calls the same way it spans separately-loaded images. A regression here
 * silently re-enables double injection (double icon swizzle / duplicated pane controls). */
static void run_tests(void) {
    /* Deterministic start: clear the marker regardless of the caller's inherited environment. */
    unsetenv(MAVERICKS_SINGLE_LOAD_ENV);

    /* First loader wins. */
    CHECK_EQ(mavericks_claim_single_load(), 1);

    /* Every subsequent copy bails. */
    CHECK_EQ(mavericks_claim_single_load(), 0);
    CHECK_EQ(mavericks_claim_single_load(), 0);

    /* Simulate a fresh process by clearing the marker: the next loader can then claim again. */
    unsetenv(MAVERICKS_SINGLE_LOAD_ENV);
    CHECK_EQ(mavericks_claim_single_load(), 1);

    /* Regression (2026-07-06): an INHERITED marker carries a FOREIGN pid (a child's pid != its
     * parent's), or an older payload's presence-only "1". It must NOT be mistaken for an in-process
     * claim — presence-only made whole fresh System Preferences instances stay inert (the About tab
     * vanished across reopens). Seed a foreign marker; the next claim must still WIN, and only then do
     * same-process copies bail. */
    setenv(MAVERICKS_SINGLE_LOAD_ENV, "1", 1);          /* foreign/stale marker (also the old presence value) */
    CHECK_EQ(mavericks_claim_single_load(), 1);         /* ignore the foreign marker, claim fresh */
    CHECK_EQ(mavericks_claim_single_load(), 0);         /* now keyed to our pid -> siblings bail */
}

TEST_MAIN()
