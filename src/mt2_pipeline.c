#include "mt2_pipeline.h"
int mt2_settle_passed(uint32_t now_ms, uint32_t settle_until_ms) {
    return now_ms >= settle_until_ms ? 1 : 0;
}

void mt2_drop_lifted(touch_frame_t *frame) {
    int kept = 0;
    for (int i = 0; i < frame->ntouches; i++) {
        if (frame->touches[i].size > 0) {
            if (kept != i) frame->touches[kept] = frame->touches[i];
            kept++;
        }
    }
    frame->ntouches = kept;
}
