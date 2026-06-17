#include "../src/mt2_pipeline.h"
#include "test.h"

static void run_tests(void) {
    CHECK_EQ(mt2_settle_passed(0, 2500), 0);
    CHECK_EQ(mt2_settle_passed(2499, 2500), 0);
    CHECK_EQ(mt2_settle_passed(2500, 2500), 1);
    CHECK_EQ(mt2_settle_passed(9999, 2500), 1);

    touch_frame_t f = {0};
    f.ntouches = 3;
    f.touches[0].size = 25; f.touches[0].x = 10;
    f.touches[1].size = 0;  f.touches[1].x = 20;
    f.touches[2].size = 12; f.touches[2].x = 30;
    mt2_drop_lifted(&f);
    CHECK_EQ(f.ntouches, 2);
    CHECK_EQ(f.touches[0].x, 10);
    CHECK_EQ(f.touches[1].x, 30);
    touch_frame_t up = {0}; up.ntouches = 1; up.touches[0].size = 0;
    mt2_drop_lifted(&up);
    CHECK_EQ(up.ntouches, 0);

    unsigned last = 0, mask = 0xdead;
    CHECK_EQ(mt2_click_changed(0, 0, &last, &mask), 0);
    CHECK_EQ(mt2_click_changed(1, 1, &last, &mask), 1); CHECK_EQ(mask, 0x1u);
    CHECK_EQ(mt2_click_changed(1, 1, &last, &mask), 0);
    CHECK_EQ(mt2_click_changed(0, 0, &last, &mask), 1); CHECK_EQ(mask, 0x0u);
    last = 0;
    CHECK_EQ(mt2_click_changed(1, 2, &last, &mask), 1); CHECK_EQ(mask, 0x2u);

    mt2_decel_t d;
    touch_frame_t held = {0}; held.ntouches = 1; held.touches[0].size = 20; held.touches[0].x = 77;
    mt2_decel_arm(&d, &held);
    CHECK_EQ(d.step, 0);
    touch_frame_t out; int has; uint32_t rearm;
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 1); CHECK_EQ(out.touches[0].x, 77); CHECK_EQ(rearm, MT2_DECEL_MS);
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 1); CHECK_EQ(rearm, MT2_DECEL_MS);
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 1); CHECK_EQ(out.ntouches, 0); CHECK_EQ(rearm, 0u);
    mt2_decel_step(&d, &out, &has, &rearm); CHECK_EQ(has, 0); CHECK_EQ(rearm, 0u);
}
TEST_MAIN()
