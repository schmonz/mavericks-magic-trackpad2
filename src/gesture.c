#include "gesture.h"
#include <stdlib.h>
#include <string.h>

/* Sensitivity divisors (MT2 device units -> mouse units). Tunable. */
#define POINTER_DIV 3
#define SCROLL_DIV  18

struct gesture_state {
    int prev_n;          /* active (down) touch count last frame */
    int prev_id;         /* primary finger id (1-finger tracking) */
    int prev_x, prev_y;  /* primary finger position */
    int prev_cx, prev_cy;/* centroid (2-finger tracking) */
    unsigned prev_buttons;
};

gesture_state_t *gesture_create(void) {
    return calloc(1, sizeof(gesture_state_t));
}
void gesture_destroy(gesture_state_t *st) { free(st); }

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int gesture_process(gesture_state_t *st, const touch_frame_t *frame, mouse_report_t *out) {
    memset(out, 0, sizeof(*out));

    int n = 0;
    long sx = 0, sy = 0;
    int pid = -1, px = 0, py = 0;
    for (int i = 0; i < frame->ntouches; i++) {
        const touch_t *t = &frame->touches[i];
        if (t->state != TS_TOUCHING) continue;
        n++; sx += t->x; sy += t->y;
        if (pid < 0) { pid = t->id; px = t->x; py = t->y; }
    }

    /* Physical click maps to left (1 finger) or right (2+ fingers). */
    unsigned buttons = 0;
    if (frame->button) buttons = (n >= 2) ? 0x2 : 0x1;
    out->buttons = buttons;

    int motion = 0;
    if (n == 1 && st->prev_n == 1 && pid == st->prev_id) {
        out->dx = clampi((px - st->prev_x) / POINTER_DIV, -127, 127);
        out->dy = clampi((py - st->prev_y) / POINTER_DIV, -127, 127);   /* MT2 y already screen-oriented */
        if (out->dx || out->dy) motion = 1;
    } else if (n == 2 && st->prev_n == 2) {
        int cx = (int)(sx / 2), cy = (int)(sy / 2);
        out->wheel_v = clampi(-(cy - st->prev_cy) / SCROLL_DIV, -127, 127);
        out->wheel_h = clampi((cx - st->prev_cx) / SCROLL_DIV, -127, 127);
        if (out->wheel_v || out->wheel_h) motion = 1;
    }

    st->prev_n = n;
    st->prev_id = pid;
    st->prev_x = px; st->prev_y = py;
    st->prev_cx = n ? (int)(sx / n) : 0;
    st->prev_cy = n ? (int)(sy / n) : 0;

    int changed = motion || (buttons != st->prev_buttons);
    st->prev_buttons = buttons;
    return changed ? 1 : 0;
}
