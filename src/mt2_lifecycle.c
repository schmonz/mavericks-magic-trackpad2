#include "mt2_lifecycle.h"

void mt2_lifecycle_reset(mt2_lifecycle_t *lc) {
    lc->prev_ids = 0;
    /* last[] need not be cleared: it is only read for ids set in prev_ids. */
}

void mt2_lifecycle_step(mt2_lifecycle_t *lc, touch_frame_t *frame) {
    uint32_t now_ids = 0;

    /* Pass 1: state by PRESENCE, not by the device's per-frame state bits. The MT2
     * device reports a transition state on touchdown that mt2_decode mislabels TS_END;
     * surfacing that as a lift means the recognizer never sees MakeTouch and never
     * commits a tap. A contact that is present here (it survived mt2_drop_lifted, which
     * keys on size) is touching: its first frame for an id is TS_START (MakeTouch),
     * later frames are TS_TOUCHING. Only a VANISHED contact (pass 2) is a real lift. */
    for (int i = 0; i < frame->ntouches; i++) {
        int id = frame->touches[i].id & 0x0f;
        uint32_t bit = (uint32_t)1u << id;
        frame->touches[i].state = (lc->prev_ids & bit) ? TS_TOUCHING : TS_START;
        now_ids |= bit;
        lc->last[id] = frame->touches[i];
    }

    /* Pass 2: append a BreakTouch (TS_END) for every id present last frame but gone
     * now, at its last-known position, so the recognizer sees a clean lift. */
    for (int id = 0; id < MAX_TOUCHES; id++) {
        uint32_t bit = (uint32_t)1u << id;
        if ((lc->prev_ids & bit) && !(now_ids & bit)) {
            if (frame->ntouches < MAX_TOUCHES) {
                touch_t end = lc->last[id];
                end.state = TS_END;
                frame->touches[frame->ntouches++] = end;
            }
        }
    }

    /* Ended ids are dropped from history (end delivered once); present ids carry over. */
    lc->prev_ids = now_ids;
}

int mt2_lifecycle_flush(mt2_lifecycle_t *lc, touch_frame_t *out) {
    out->ntouches = 0;
    out->button = 0;
    out->timestamp = 0;
    for (int id = 0; id < MAX_TOUCHES; id++) {
        if (lc->prev_ids & ((uint32_t)1u << id)) {
            touch_t end = lc->last[id];
            end.state = TS_END;
            out->touches[out->ntouches++] = end;
        }
    }
    lc->prev_ids = 0;
    return out->ntouches > 0;
}
