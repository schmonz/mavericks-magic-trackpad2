#include "mt2_session.h"

void mt2_session_connect(mt2_session_t *s, uintptr_t source,
                         mt2_transport_mode_t mode, uint32_t now_ms) {
    s->active_source = source;
    s->mode = mode;
    s->settle_until_ms = now_ms + MT2_SETTLE_MS;
    s->last_button = 0;
    mt2_lifecycle_reset(&s->lifecycle);      /* next contacts read as new; none pending an end */
}

static void emit(mt2_session_t *s, const touch_frame_t *tf,
                 const mt2_session_sink_t *sink) {
    unsigned mask = 0;
    if (mt2_click_changed((unsigned)tf->button, tf->ntouches, &s->last_button, &mask))
        sink->post_click(sink->ctx, mask);
    sink->feed_frame(sink->ctx, tf);
}

void mt2_session_frame(mt2_session_t *s, uintptr_t source,
                       const touch_frame_t *tf, uint32_t now_ms,
                       const mt2_session_sink_t *sink) {
    if (source != s->active_source) return;                       /* single-active guard */
    if (!mt2_settle_passed(now_ms, s->settle_until_ms)) return;   /* settle gate */

    /* Unified lifecycle path (both transports): reduce to the real (present) contacts,
       then synthesize the MakeTouch/BreakTouch transitions the gesture recognizer needs
       for tap-to-click. A vanished contact is emitted once as BreakTouch (TS_END) at its
       last-known position -- the native clean-lift signal that also prevents fling, so no
       held-replay deceleration is needed. */
    touch_frame_t f = *tf;
    mt2_drop_lifted(&f);                          /* keep only real (size>0) contacts */
    mt2_lifecycle_step(&s->lifecycle, &f);        /* +START for new, +BreakTouch for vanished */
    if (f.ntouches > 0) emit(s, &f, sink);

    /* While a contact is still down, keep a silence watchdog: if the stream stops with no
       lift frame, the timer flushes the outstanding BreakTouch so the lift still registers. */
    if (s->lifecycle.prev_ids) sink->arm_timer(sink->ctx, MT2_IDLE_MS);
}

void mt2_session_timer(mt2_session_t *s, const mt2_session_sink_t *sink) {
    /* Silence watchdog fired: deliver BreakTouch for any contact still marked down.
       After a reconnect, mt2_session_connect reset the lifecycle (prev_ids = 0), so a
       timer armed for a prior connection flushes nothing -- a harmless no-op. */
    touch_frame_t end;
    if (mt2_lifecycle_flush(&s->lifecycle, &end))
        emit(s, &end, sink);
}
