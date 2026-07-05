#include "../src/voodoo_input.h"
#include "test.h"

static void run_tests(void) {
    VoodooInputEvent f = {0};
    f.contact_count = 1;
    f.transducers[0].id = 3;
    f.transducers[0].state = TS_TOUCHING;
    CHECK_EQ(f.contact_count, 1);
    CHECK_EQ(f.transducers[0].id, 3);
    CHECK_EQ(f.transducers[0].state, TS_TOUCHING);
    CHECK_EQ(MAX_TOUCHES, 16);
}
TEST_MAIN()
