#include "../examples/VoodooInputSample/vinput_demo_path.h"
#include "test.h"

static void run_tests(void) {
    unsigned x = 0, y = 0;
    const unsigned LMX = 1000, LMY = 1000;

    /* Every phase stays inside [0..lmax]. */
    for (unsigned p = 0; p < 240; p++) {
        vinput_demo_point(p, LMX, LMY, &x, &y);
        CHECK(x <= LMX);
        CHECK(y <= LMY);
    }

    /* It actually MOVES: two phases give different points. */
    unsigned x0, y0, x1, y1;
    vinput_demo_point(0,  LMX, LMY, &x0, &y0);
    vinput_demo_point(30, LMX, LMY, &x1, &y1);
    CHECK(x0 != x1 || y0 != y1);

    /* Periodic: a full period returns to the start. */
    unsigned xp, yp;
    vinput_demo_point(VINPUT_DEMO_PERIOD, LMX, LMY, &xp, &yp);
    CHECK_EQ(xp, x0);
    CHECK_EQ(yp, y0);
}
TEST_MAIN()
