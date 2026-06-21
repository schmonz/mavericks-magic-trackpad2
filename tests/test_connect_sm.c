#include "../src/mt2_connect_sm.h"
#include "test.h"

static void run_tests(void) {
    /* From IDLE, a control-channel attach advances to CONTROL_UP and asks us to wait for OPEN. */
    csm_result_t r = csm_step(CSM_IDLE, CSM_EV_CONTROL_ATTACH);
    CHECK_EQ(r.next, CSM_CONTROL_UP);
    CHECK_EQ(r.action, CSM_ACT_WAIT_CONTROL_OPEN);
}
TEST_MAIN()
