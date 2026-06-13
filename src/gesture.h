#ifndef GESTURE_H
#define GESTURE_H
#include "touch_model.h"

/* A synthesized relative-pointer event derived from MT2 touch frames. */
typedef struct {
    int dx, dy;          /* relative pointer motion (mouse units) */
    int wheel_v;         /* vertical scroll (mouse wheel units) */
    int wheel_h;         /* horizontal scroll */
    unsigned buttons;    /* bit0 = left, bit1 = right */
} mouse_report_t;

typedef struct gesture_state gesture_state_t;

gesture_state_t *gesture_create(void);
void gesture_destroy(gesture_state_t *st);

/* Consume one decoded touch frame; fill *out with the synthesized mouse report.
 * Always writes *out (zeroed when idle). Returns 1 if the report is meaningful
 * (nonzero motion/scroll or a button change) and should be sent, else 0. */
int gesture_process(gesture_state_t *st, const touch_frame_t *frame, mouse_report_t *out);

#endif
