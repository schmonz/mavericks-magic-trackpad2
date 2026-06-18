#include "mt2_lifecycle.h"

void mt2_lifecycle_reset(mt2_lifecycle_t *lc) {
    lc->prev_ids = 0;
}

void mt2_lifecycle_mark(mt2_lifecycle_t *lc, touch_frame_t *frame) {
    uint32_t now_ids = 0;
    for (int i = 0; i < frame->ntouches; i++) {
        int id = frame->touches[i].id & 0x1f;     /* device id is a low nibble */
        uint32_t bit = (uint32_t)1u << id;
        if (frame->touches[i].state == TS_TOUCHING && !(lc->prev_ids & bit))
            frame->touches[i].state = TS_START;
        now_ids |= bit;
    }
    lc->prev_ids = now_ids;
}
