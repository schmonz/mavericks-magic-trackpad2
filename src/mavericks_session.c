#include "mavericks_session.h"

/* ===========================================================================
 * mavericks_session — the CONDITION stage of the input pipeline.
 *
 * The driver does three jobs; only ONE of them lives here:
 *   TRANSLATE  faithfully reframe the MT2's native frames into the report
 *              Apple's genuine driver expects (mt2_decode -> mavericks_session ->
 *              mavericks_amd_construct_report / mt2_usb_bytes). We change FORMAT, never meaning.
 *   CONDITION  the only thing we SHAPE: settle gate, contact lifecycle
 *              (MakeTouch/BreakTouch), liftoff shaping, silence watchdog.
 *              This file. We shape the frame STREAM so Apple's recognizer can
 *              run cleanly; we never invent contacts, clicks, or gestures.
 *   INJECT     what the device's wire can't carry but Apple's path needs:
 *              sensor geometry and the device-button gate, seeded into the
 *              genuine driver by the readers (see MT2USBReader/MT2BTReader).
 *
 * Recognition — cursor, gestures, tap-to-click, right-click — is entirely
 * Apple's. We hand its recognizer a faithful, well-conditioned stream and get
 * out of the way. See docs/mt-stack/explanation.md (load-bearing principle).
 * =========================================================================== */

/* The single MT2 conditioning policy — both transports converged to it. */
const mavericks_session_policy_t mt2_policy_default = { MT2_LIFTOFF_ABSENCE_PAIR, /*empties*/0, /*watchdog*/1 };

void mavericks_session_connect(mavericks_session_t *s, uintptr_t source,
                         mt2_transport_mode_t mode,
                         const mavericks_session_policy_t *policy, uint32_t now_ms) {
    s->active_source = source;
    s->mode = mode;
    s->policy = *policy;
    s->settle_until_ms = now_ms + MT2_SETTLE_MS;
    s->last_button = 0;
    mavericks_lifecycle_reset(&s->lifecycle);      /* next contacts read as new; none pending an end */
}

/* Deliver one conditioned frame to Apple's recognizer. If the device's REAL
   physical-button bit changed since the last frame, forward that edge first —
   the mask is a faithful mapping of the hardware button + finger count to Apple's
   primary/secondary click code, NOT a synthesized click. */
static void emit(mavericks_session_t *s, const MavericksTouchFrame *tf,
                 const mavericks_session_sink_t *sink) {
    unsigned mask = 0;
    if (mt2_button_edge((unsigned)tf->isPhysicalButtonDown, tf->contact_count, &s->last_button, &mask))
        sink->post_button_edge(sink->ctx, mask);
    sink->feed_frame(sink->ctx, tf);
}

/* 1 iff the frame is a PURE lift -- has contacts and every one is BreakTouch (TS_END), i.e. a
   full hand-off-the-pad with nothing still down. (A partial lift -- some present + some TS_END --
   is not "all".) */
static int frame_all_breaktouch(const MavericksTouchFrame *f) {
    if (f->contact_count == 0) return 0;
    for (int i = 0; i < (int)f->contact_count; i++)
        if (f->transducers[i].state != TS_END) return 0;
    return 1;
}

/* CONDITIONING, not click synthesis: the tap-to-click decision is entirely Apple's
   recognizer's; here we only shape WHEN it sees the lift so it can finalize its own tap.

   On a FULL lift, emit a zero-contact (absence) frame -- NOT the BreakTouch frame then an absence
   frame -- and then a SECOND absence frame as a "pump".

   The first absence frame is what finalizes the path liftoff AND is required for tap recognition
   (verified hands-free with tools/iter_tap.sh: dropping it collapses selectTapChord to ~0; the
   recognizer needs to observe the contact ABSENT to finalize the tap). Emitting a BreakTouch frame
   AND an absence frame instead made the native recognizer fire MTChordCycling::handleChordLiftoff
   TWICE per tap (~0ms apart -- both frames are fed back-to-back), which destabilises the tap-drag
   cycle and the click commits erratically. So the lift transition is signalled by the absence
   alone (iter_tap.sh: handleChordLiftoff 2N -> N, selectTapChord still N).

   The SECOND absence is a PUMP. After a tap the recognizer ARMS a "waiting click" and only
   re-checks its flush timeout when a FRAME arrives; with no frame after the lift it never flushes,
   so the click batches onto the NEXT tap's frames as a ~6ms phantom double-click (and a final tap
   with nothing after it drops entirely). Our pipeline goes silent after the lift -- even idle
   device reports are dropped (contact_count==0 emits nothing in mavericks_session_frame) -- so without this
   the recognizer falls into the erratic MTTapDragManager::handleTapsForDrag drag-cycle path. The
   2nd absence gives it the cycle to flush cleanly via MTTapDragManager::sendWaitingClickAtHalfTimeout
   -- one clean click per tap. Verified hands-free: tools/trace_btnstack.d shows every button post
   routed through sendWaitingClickAtHalfTimeout (8 taps -> 16 posts, 2/tap) and tools/tap_clicks.sh
   shows the phantom gone. Two ABSENCE frames do NOT re-double handleChordLiftoff: only the first is
   a lift transition; the second is "still no contact".

   A partial lift still emits the frame as-is (present contacts + the lifting one's TS_END); there
   are still contacts down, so no absence and no full liftoff. Shared path => both transports. */
static void emit_with_liftoff(mavericks_session_t *s, const MavericksTouchFrame *tf,
                              const mavericks_session_sink_t *sink) {
    if (frame_all_breaktouch(tf)) {
        MavericksTouchFrame empty = {0};
        emit(s, &empty, sink);   /* finalize the path liftoff */
        emit(s, &empty, sink);   /* pump: flush the armed tap-click this tap, not the next */
    } else {
        emit(s, tf, sink);
    }
}

void mavericks_session_frame(mavericks_session_t *s, uintptr_t source,
                       const MavericksTouchFrame *tf, uint32_t now_ms,
                       const mavericks_session_sink_t *sink) {
    if (source != s->active_source) return;                       /* single-active guard */
    if (!mt2_settle_passed(now_ms, s->settle_until_ms)) return;   /* settle gate */

    /* Unified lifecycle path (both transports): reduce to the real (present) contacts,
       then synthesize the MakeTouch/BreakTouch transitions the gesture recognizer needs
       for tap-to-click. A vanished contact is emitted once as BreakTouch (TS_END) at its
       last-known position -- the native clean-lift signal that also prevents fling, so no
       held-replay deceleration is needed. */
    MavericksTouchFrame f = *tf;
    mt2_drop_lifted(&f);                          /* keep only real (size>0) contacts */
    mavericks_lifecycle_step(&s->lifecycle, &f);        /* +START for new, +BreakTouch for vanished */
    if (f.contact_count > 0) {
        if (s->policy.liftoff_shape == MT2_LIFTOFF_ABSENCE_PAIR)
            emit_with_liftoff(s, &f, sink);
        else
            emit(s, &f, sink);              /* PASSTHROUGH: the BreakTouch frame as-is */
    } else if (s->policy.emit_empty_frames) {
        emit(s, &f, sink);                  /* zero-contact frame reaches the sink (click edge too) */
    }

    /* While a contact is still down, keep a silence watchdog: if the stream stops with no
       lift frame, the timer flushes the outstanding BreakTouch so the lift still registers.
       (Policy-gated: the USB row runs without it today — its absence pump covers the gap.) */
    if (s->policy.arm_watchdog && s->lifecycle.prev_ids)
        sink->arm_timer(sink->ctx, MT2_IDLE_MS);
}

void mavericks_session_timer(mavericks_session_t *s, const mavericks_session_sink_t *sink) {
    /* Silence watchdog fired: deliver BreakTouch for any contact still marked down.
       After a reconnect, mavericks_session_connect reset the lifecycle (prev_ids = 0), so a
       timer armed for a prior connection flushes nothing -- a harmless no-op.
       NB the flush emits the ABSENCE_PAIR shape regardless of policy.liftoff_shape —
       valid while every arm_watchdog row also uses ABSENCE_PAIR (true for both shipped
       rows; revisit if a row ever arms the watchdog with PASSTHROUGH). */
    MavericksTouchFrame end = {0};
    if (mavericks_lifecycle_flush(&s->lifecycle, &end))
        emit_with_liftoff(s, &end, sink);
}
