#include "gesture.h"
#include <stdlib.h>
#include <string.h>

/* Sensitivity divisors (MT2 device units -> mouse units). Tunable.
 * Scroll smooths the centroid velocity with an exponential moving average
 * (SCROLL_ALPHA; lower = smoother, slower to react to speed changes) and
 * accumulates it (carrying the remainder) so slow movement isn't lost to
 * integer truncation. SCROLL_DIV sets the overall scroll rate. */
#define POINTER_DIV 3
#define SCROLL_DIV  110.0
#define SCROLL_ALPHA 0.25

/* Tap-to-click thresholds. */
#define TAP_MAX_SECONDS 0.30   /* a tap must be briefer than this */
#define TAP_MOVE_THRESH 130    /* ...and move less than this (MT2 units) */

struct gesture_state {
    int prev_n;          /* active (down) touch count last frame */
    int prev_id;         /* primary finger id (1-finger tracking) */
    int prev_x, prev_y;  /* primary finger position */
    int prev_cx, prev_cy;/* centroid (2-finger tracking) */
    double smooth_v, smooth_h; /* low-pass-filtered scroll velocity */
    double accum_v, accum_h;   /* scroll remainder carried between frames */
    unsigned prev_buttons;

    /* tap tracking across a touch sequence */
    int in_touch;
    double touch_start_t;
    int touch_start_x, touch_start_y;
    int touch_max_fingers;
    int touch_moved;
    int touch_phys_click;
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
        /* low-pass the velocity so abrupt speed changes ramp in gradually */
        st->smooth_v += ((cy - st->prev_cy) - st->smooth_v) * SCROLL_ALPHA;
        st->smooth_h += ((cx - st->prev_cx) - st->smooth_h) * SCROLL_ALPHA;
        st->accum_v += st->smooth_v;            /* accumulate smoothed distance... */
        st->accum_h += st->smooth_h;
        int tv = (int)(st->accum_v / SCROLL_DIV); /* ...emit whole ticks... */
        int th = (int)(st->accum_h / SCROLL_DIV);
        st->accum_v -= tv * SCROLL_DIV;          /* ...carry the remainder. */
        st->accum_h -= th * SCROLL_DIV;
        out->wheel_v = clampi(-tv, -127, 127);
        out->wheel_h = clampi(th, -127, 127);
        if (out->wheel_v || out->wheel_h) motion = 1;
    }
    if (!(n == 2 && st->prev_n == 2)) {          /* reset between scrolls */
        st->accum_v = st->accum_h = 0;
        st->smooth_v = st->smooth_h = 0;
    }

    /* Tap-to-click: track the touch sequence; on lift, if it was brief, didn't
     * move, and had no physical click, emit a momentary click. */
    unsigned tap = 0;
    if (n > 0) {
        if (!st->in_touch) {
            st->in_touch = 1;
            st->touch_start_t = frame->timestamp;
            st->touch_start_x = px; st->touch_start_y = py;
            st->touch_max_fingers = n;
            st->touch_moved = 0;
            st->touch_phys_click = 0;
        } else {
            if (n > st->touch_max_fingers) st->touch_max_fingers = n;
            int ddx = px - st->touch_start_x, ddy = py - st->touch_start_y;
            if (ddx * ddx + ddy * ddy > TAP_MOVE_THRESH * TAP_MOVE_THRESH)
                st->touch_moved = 1;
        }
        if (frame->button) st->touch_phys_click = 1;
    } else if (st->in_touch) {
        double dt = frame->timestamp - st->touch_start_t;
        if (!st->touch_moved && !st->touch_phys_click && dt <= TAP_MAX_SECONDS)
            tap = (st->touch_max_fingers >= 2) ? 0x2 : 0x1;
        st->in_touch = 0;
    }
    out->tap = tap;

    st->prev_n = n;
    st->prev_id = pid;
    st->prev_x = px; st->prev_y = py;
    st->prev_cx = n ? (int)(sx / n) : 0;
    st->prev_cy = n ? (int)(sy / n) : 0;

    int changed = motion || tap || (buttons != st->prev_buttons);
    st->prev_buttons = buttons;
    return changed ? 1 : 0;
}
