#include "../src/gesture.h"
#include "test.h"

static touch_frame_t mk1(int id, int x, int y, int button) {
    touch_frame_t f = {0};
    f.ntouches = 1; f.button = button;
    f.touches[0].id = id; f.touches[0].state = TS_TOUCHING;
    f.touches[0].x = x; f.touches[0].y = y;
    return f;
}
static touch_frame_t mk2(int x1, int y1, int x2, int y2, int button) {
    touch_frame_t f = {0};
    f.ntouches = 2; f.button = button;
    f.touches[0].id = 1; f.touches[0].state = TS_TOUCHING; f.touches[0].x = x1; f.touches[0].y = y1;
    f.touches[1].id = 2; f.touches[1].state = TS_TOUCHING; f.touches[1].x = x2; f.touches[1].y = y2;
    return f;
}

static void run_tests(void) {
    mouse_report_t r;

    /* One-finger motion: first frame is baseline (no motion), second moves. */
    {
        gesture_state_t *st = gesture_create();
        touch_frame_t a = mk1(1, 0, 0, 0);
        CHECK_EQ(gesture_process(st, &a, &r), 0);           /* baseline: no motion */
        touch_frame_t b = mk1(1, 400, 0, 0);
        CHECK_EQ(gesture_process(st, &b, &r), 1);
        CHECK(r.dx > 0 && r.dx <= 127);                     /* +x -> cursor right */
        CHECK_EQ(r.dy, 0);
        touch_frame_t c = mk1(1, 400, 400, 0);
        gesture_process(st, &c, &r);
        CHECK(r.dy > 0);                                    /* +y tracks screen-down (verified on HW) */
        gesture_destroy(st);
    }

    /* Two-finger scroll. */
    {
        gesture_state_t *st = gesture_create();
        touch_frame_t d = mk2(0, 0, 100, 0, 0);
        CHECK_EQ(gesture_process(st, &d, &r), 0);           /* baseline */
        touch_frame_t e = mk2(0, 400, 100, 400, 0);
        CHECK_EQ(gesture_process(st, &e, &r), 1);
        CHECK(r.wheel_v != 0);
        CHECK_EQ(r.dx, 0);                                  /* scroll, not pointer */
        gesture_destroy(st);
    }

    /* Physical click: 1 finger -> left, 2 fingers -> right. */
    {
        gesture_state_t *st = gesture_create();
        touch_frame_t f1 = mk1(1, 0, 0, 1);
        CHECK_EQ(gesture_process(st, &f1, &r), 1);
        CHECK(r.buttons & 0x1);
        CHECK(!(r.buttons & 0x2));
        gesture_destroy(st);

        gesture_state_t *st2 = gesture_create();
        touch_frame_t f2 = mk2(0, 0, 100, 0, 1);
        CHECK_EQ(gesture_process(st2, &f2, &r), 1);
        CHECK(r.buttons & 0x2);                             /* two-finger click -> right */
        gesture_destroy(st2);
    }

    /* Finger-count change must not emit a motion spike. */
    {
        gesture_state_t *st = gesture_create();
        touch_frame_t a = mk1(1, 0, 0, 0);
        gesture_process(st, &a, &r);
        touch_frame_t two = mk2(500, 500, 600, 500, 0);     /* jump to 2 fingers far away */
        gesture_process(st, &two, &r);
        CHECK_EQ(r.dx, 0); CHECK_EQ(r.dy, 0); CHECK_EQ(r.wheel_v, 0);
        gesture_destroy(st);
    }
}
TEST_MAIN()
