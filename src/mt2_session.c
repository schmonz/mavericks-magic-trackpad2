#include "mt2_session.h"

void mt2_session_connect(mt2_session_t *s, uintptr_t source,
                         mt2_transport_mode_t mode, uint32_t now_ms) {
    s->active_source = source;
    s->mode = mode;
    s->settle_until_ms = now_ms + MT2_SETTLE_MS;
    s->last_button = 0;
    s->decel.step = 3;                       /* idle until a contact arms it */
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
    if (s->mode == MT2_STREAMING) { emit(s, tf, sink); return; }  /* pass-through */

    /* EVENT_DRIVEN: lift-drop + idle-decel/clean-lift (decel replays in Task 6) */
    {
        touch_frame_t f = *tf;
        mt2_drop_lifted(&f);
        if (f.ntouches > 0) {
            emit(s, &f, sink);
            mt2_decel_arm(&s->decel, &f);
            sink->arm_timer(sink->ctx, MT2_IDLE_MS);
        } else {
            s->decel.step = 0;               /* liftoff: keep last held, restart decel */
            sink->arm_timer(sink->ctx, MT2_IDLE_MS);
        }
    }
}

void mt2_session_timer(mt2_session_t *s, const mt2_session_sink_t *sink) {
    touch_frame_t out; int has = 0; uint32_t rearm = 0;
    mt2_decel_step(&s->decel, &out, &has, &rearm);
    if (has) emit(s, &out, sink);            /* held replay / clean lift (click re-checked, no-op) */
    if (rearm) sink->arm_timer(sink->ctx, rearm);
}
