#include "mavericks_pipeline.h"
int mavericks_settle_passed(uint32_t now_ms, uint32_t settle_until_ms) {
    return now_ms >= settle_until_ms ? 1 : 0;
}

void mavericks_drop_lifted(MavericksTouchFrame *frame) {
    int kept = 0;
    for (int i = 0; i < (int)frame->contact_count; i++) {
        if (frame->transducers[i].currentCoordinates.pressure > 0) {
            if (kept != i) frame->transducers[kept] = frame->transducers[i];
            kept++;
        }
    }
    frame->contact_count = kept;
}

int mavericks_button_edge(unsigned button, int nfingers, unsigned *last_button,
                      unsigned *out_mask) {
    button = button ? 1u : 0u;
    if (button == *last_button) return 0;
    *last_button = button;
    *out_mask = button ? (nfingers == 2 ? 0x2u : 0x1u) : 0x0u;
    return 1;
}

void mavericks_decel_arm(mavericks_decel_t *d, const MavericksTouchFrame *held) {
    d->held = *held;
    d->step = 0;
}
void mavericks_decel_step(mavericks_decel_t *d, MavericksTouchFrame *out,
                    int *out_has_frame, uint32_t *out_rearm_ms) {
    if (d->step < 2) {
        *out = d->held;
        *out_has_frame = 1;
        *out_rearm_ms = MAVERICKS_DECEL_MS;
        d->step++;
    } else if (d->step == 2) {
        out->contact_count = 0; out->isPhysicalButtonDown = 0; out->timestamp = 0;
        *out_has_frame = 1;
        *out_rearm_ms = 0;
        d->step = 3;
    } else {
        *out_has_frame = 0;
        *out_rearm_ms = 0;
    }
}
