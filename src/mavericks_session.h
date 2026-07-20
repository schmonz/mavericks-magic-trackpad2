#ifndef MAVERICKS_SESSION_H
#define MAVERICKS_SESSION_H
#include "mavericks_pipeline.h"
#include "mavericks_lifecycle.h"
#include "mavericks_frame.h"
#include <stdint.h>

/* Effects seam: the session never touches IOKit/clock. The shell implements these
   to drive IOKit; tests implement them to record. feed_frame gets a MavericksTouchFrame
   (the shell encodes + feeds). CONTRACT: all three callbacks must be non-NULL —
   the session calls them on hot paths with no NULL checks. */
typedef struct {
    void (*post_button_edge)(void *ctx, unsigned mask);
    void (*feed_frame)(void *ctx, const MavericksTouchFrame *frame);
    void (*arm_timer)(void *ctx, uint32_t ms);
    void *ctx;
} mavericks_session_sink_t;

/* Per-transport conditioning policy: the THREE observed deltas between the BT and USB
   assemblies, expressed as data so each later convergence is a one-line flip with a
   known test (see the engine-unification design spec). Nothing speculative: one field
   per delta the seam map found, no more. */
typedef enum {
    MAVERICKS_LIFTOFF_ABSENCE_PAIR = 0,  /* full lift -> two zero-contact frames (BT-proven shape) */
    MAVERICKS_LIFTOFF_PASSTHROUGH  = 1,  /* full lift -> the BreakTouch frame as-is */
} mavericks_liftoff_shape_t;

typedef struct {
    mavericks_liftoff_shape_t liftoff_shape;
    uint8_t emit_empty_frames;   /* 1: zero-contact frames reach the sink; 0: dropped */
    uint8_t arm_watchdog;        /* 1: arm the MAVERICKS_IDLE_MS silence flush while contacts are down */
} mavericks_session_policy_t;

/* The shipped row (defined in mavericks_session.c so host tests drive the exact row the
   kext registers). mavericks_policy_default = the single MT2 conditioning policy — both
   transports converged to it (ABSENCE_PAIR liftoff, no empty-frame forwarding, silence
   watchdog); a future device would supply its own row. */
extern const mavericks_session_policy_t mavericks_policy_default;

typedef struct {
    uintptr_t active_source;
    mavericks_transport_mode_t mode;
    uint32_t settle_until_ms;
    unsigned last_button;
    mavericks_session_policy_t policy;
    mavericks_lifecycle_t lifecycle;   /* MakeTouch/BreakTouch lifecycle synthesis */
} mavericks_session_t;

/* policy must be non-NULL — pass a shipped row (mavericks_policy_default). */
void mavericks_session_connect(mavericks_session_t *s, uintptr_t source,
                         mavericks_transport_mode_t mode,
                         const mavericks_session_policy_t *policy, uint32_t now_ms);
void mavericks_session_frame(mavericks_session_t *s, uintptr_t source,
                       const MavericksTouchFrame *tf, uint32_t now_ms,
                       const mavericks_session_sink_t *sink);
void mavericks_session_timer(mavericks_session_t *s, const mavericks_session_sink_t *sink);
#endif
