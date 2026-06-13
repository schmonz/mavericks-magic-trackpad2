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

    /* Two-finger scroll: smoothing ramps up, so a tick emits within a few
     * frames of sustained movement (not necessarily the first). */
    {
        gesture_state_t *st = gesture_create();
        touch_frame_t base = mk2(0, 0, 100, 0, 0);
        CHECK_EQ(gesture_process(st, &base, &r), 0);        /* baseline */
        int saw_scroll = 0;
        for (int i = 1; i <= 8; i++) {
            touch_frame_t f = mk2(0, i * 200, 100, i * 200, 0);
            gesture_process(st, &f, &r);
            CHECK_EQ(r.dx, 0);                              /* scroll, not pointer */
            if (r.wheel_v != 0) saw_scroll = 1;
        }
        CHECK(saw_scroll);
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

    /* Tap-to-click: brief touch with no movement -> momentary click. */
    {
        gesture_state_t *st = gesture_create();
        touch_frame_t down = mk1(1, 1000, 1000, 0); down.timestamp = 0.0;
        gesture_process(st, &down, &r);
        touch_frame_t up = {0}; up.ntouches = 0; up.timestamp = 0.10;  /* lifted 100ms later */
        CHECK_EQ(gesture_process(st, &up, &r), 1);
        CHECK_EQ(r.tap, 0x1u);                              /* one-finger tap -> left */
        gesture_destroy(st);
    }
    {   /* two-finger tap -> right */
        gesture_state_t *st = gesture_create();
        touch_frame_t d = mk2(0, 0, 100, 0, 0); d.timestamp = 0.0;
        gesture_process(st, &d, &r);
        touch_frame_t up = {0}; up.ntouches = 0; up.timestamp = 0.10;
        gesture_process(st, &up, &r);
        CHECK_EQ(r.tap, 0x2u);
        gesture_destroy(st);
    }
    {   /* moved during touch -> NOT a tap */
        gesture_state_t *st = gesture_create();
        touch_frame_t down = mk1(1, 0, 0, 0); down.timestamp = 0.0;
        gesture_process(st, &down, &r);
        touch_frame_t mv = mk1(1, 800, 800, 0); mv.timestamp = 0.05;
        gesture_process(st, &mv, &r);
        touch_frame_t up = {0}; up.ntouches = 0; up.timestamp = 0.10;
        gesture_process(st, &up, &r);
        CHECK_EQ(r.tap, 0u);
        gesture_destroy(st);
    }
    {   /* held too long -> NOT a tap */
        gesture_state_t *st = gesture_create();
        touch_frame_t down = mk1(1, 0, 0, 0); down.timestamp = 0.0;
        gesture_process(st, &down, &r);
        touch_frame_t up = {0}; up.ntouches = 0; up.timestamp = 0.50;  /* 500ms */
        gesture_process(st, &up, &r);
        CHECK_EQ(r.tap, 0u);
        gesture_destroy(st);
    }
    {   /* physical click during touch -> NOT also a tap */
        gesture_state_t *st = gesture_create();
        touch_frame_t down = mk1(1, 0, 0, 1); down.timestamp = 0.0;
        gesture_process(st, &down, &r);
        touch_frame_t up = {0}; up.ntouches = 0; up.timestamp = 0.10;
        gesture_process(st, &up, &r);
        CHECK_EQ(r.tap, 0u);
        gesture_destroy(st);
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
