#ifndef MAVERICKS_SYNTH_TEARDOWN_H
#define MAVERICKS_SYNTH_TEARDOWN_H
/* Pure teardown-ORDER contract for a fabricated AppleMultitouchDevice. The kext fills these ops
 * with real IOKit calls; host tests fill them with recorders. The ONE invariant this enforces:
 * clear the feed fence FIRST, release the workloop LAST (VoodooInput SimulatorDevice ordering).
 * Any op may be NULL (skipped) — a teardown path with no shell passes NULL for term_shell. */
typedef struct {
    void (*clear_ready)(void *ctx);   /* 1: fence — no new frame reaches the AMD after this   */
    void (*term_shell)(void *ctx);    /* 2: terminate the MT1 HID shell (may be NULL)          */
    void (*term_amd)(void *ctx);      /* 3: terminate() the AMD — Apple's clients deregister   */
    void (*release_amd)(void *ctx);   /* 4: drop our AMD ref                                   */
    void (*release_wl)(void *ctx);    /* 5: release the retained workloop LAST                 */
    void *ctx;
} mavericks_synth_teardown_ops_t;

void mavericks_synth_teardown_run(const mavericks_synth_teardown_ops_t *ops);
#endif
