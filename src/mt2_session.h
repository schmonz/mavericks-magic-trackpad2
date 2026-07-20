#ifndef MT2_SESSION_H
#define MT2_SESSION_H
#include "mt2_pipeline.h"
#include "mt2_lifecycle.h"
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
} mt2_session_sink_t;

/* Per-transport conditioning policy: the THREE observed deltas between the BT and USB
   assemblies, expressed as data so each later convergence is a one-line flip with a
   known test (see the engine-unification design spec). Nothing speculative: one field
   per delta the seam map found, no more. */
typedef enum {
    MT2_LIFTOFF_ABSENCE_PAIR = 0,  /* full lift -> two zero-contact frames (BT-proven shape) */
    MT2_LIFTOFF_PASSTHROUGH  = 1,  /* full lift -> the BreakTouch frame as-is */
} mt2_liftoff_shape_t;

typedef struct {
    mt2_liftoff_shape_t liftoff_shape;
    uint8_t emit_empty_frames;   /* 1: zero-contact frames reach the sink; 0: dropped */
    uint8_t arm_watchdog;        /* 1: arm the MT2_IDLE_MS silence flush while contacts are down */
} mt2_session_policy_t;

/* The shipped row (defined in mt2_session.c so host tests drive the exact row the
   kext registers). mt2_policy_default = the single MT2 conditioning policy — both
   transports converged to it (ABSENCE_PAIR liftoff, no empty-frame forwarding, silence
   watchdog); a future device would supply its own row. */
extern const mt2_session_policy_t mt2_policy_default;

typedef struct {
    uintptr_t active_source;
    mt2_transport_mode_t mode;
    uint32_t settle_until_ms;
    unsigned last_button;
    mt2_session_policy_t policy;
    mt2_lifecycle_t lifecycle;   /* MakeTouch/BreakTouch lifecycle synthesis */
} mt2_session_t;

/* policy must be non-NULL — pass a shipped row (mt2_policy_default). */
void mt2_session_connect(mt2_session_t *s, uintptr_t source,
                         mt2_transport_mode_t mode,
                         const mt2_session_policy_t *policy, uint32_t now_ms);
void mt2_session_frame(mt2_session_t *s, uintptr_t source,
                       const MavericksTouchFrame *tf, uint32_t now_ms,
                       const mt2_session_sink_t *sink);
void mt2_session_timer(mt2_session_t *s, const mt2_session_sink_t *sink);
#endif
