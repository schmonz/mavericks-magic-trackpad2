#include "../src/mt2_pipeline.h"
#include "test.h"

static void run_tests(void) {
    CHECK_EQ(mt2_settle_passed(0, 2500), 0);
    CHECK_EQ(mt2_settle_passed(2499, 2500), 0);
    CHECK_EQ(mt2_settle_passed(2500, 2500), 1);
    CHECK_EQ(mt2_settle_passed(9999, 2500), 1);
}
TEST_MAIN()
