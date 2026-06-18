#ifndef MT2_SESSION_H
#define MT2_SESSION_H
#include "mt2_pipeline.h"
#include "mt2_lifecycle.h"
#include "touch_model.h"
#include <stdint.h>

/* Effects seam: the session never touches IOKit/clock. The shell implements these
   to drive IOKit; tests implement them to record. feed_frame gets a touch_frame_t
   (the shell encodes + feeds). CONTRACT: all three callbacks must be non-NULL —
   the session calls them on hot paths with no NULL checks. */
typedef struct {
    void (*post_click)(void *ctx, unsigned mask);
    void (*feed_frame)(void *ctx, const touch_frame_t *frame);
    void (*arm_timer)(void *ctx, uint32_t ms);
    void *ctx;
} mt2_session_sink_t;

typedef struct {
    uintptr_t active_source;
    mt2_transport_mode_t mode;
    uint32_t settle_until_ms;
    unsigned last_button;
    mt2_lifecycle_t lifecycle;   /* MakeTouch/BreakTouch lifecycle synthesis */
} mt2_session_t;

void mt2_session_connect(mt2_session_t *s, uintptr_t source,
                         mt2_transport_mode_t mode, uint32_t now_ms);
void mt2_session_frame(mt2_session_t *s, uintptr_t source,
                       const touch_frame_t *tf, uint32_t now_ms,
                       const mt2_session_sink_t *sink);
void mt2_session_timer(mt2_session_t *s, const mt2_session_sink_t *sink);
#endif
