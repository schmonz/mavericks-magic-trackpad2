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
}
TEST_MAIN()
