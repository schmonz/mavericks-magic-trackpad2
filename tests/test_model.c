#include "../src/touch_model.h"
#include "test.h"

static void run_tests(void) {
    touch_frame_t f = {0};
    f.ntouches = 1;
    f.touches[0].id = 3;
    f.touches[0].state = TS_TOUCHING;
    CHECK_EQ(f.ntouches, 1);
    CHECK_EQ(f.touches[0].id, 3);
    CHECK_EQ(f.touches[0].state, TS_TOUCHING);
    CHECK_EQ(MAX_TOUCHES, 16);
}
TEST_MAIN()
