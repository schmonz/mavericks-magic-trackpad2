#include "../src/mt2_connect_sm.h"
#include "test.h"

static void run_tests(void) {
    /* From IDLE, a control-channel attach advances to CONTROL_UP and asks us to wait for OPEN. */
    csm_result_t r = csm_step(CSM_IDLE, CSM_EV_CONTROL_ATTACH);
    CHECK_EQ(r.next, CSM_CONTROL_UP);
    CHECK_EQ(r.action, CSM_ACT_WAIT_CONTROL_OPEN);

    /* Each observed event advances exactly one state, with the documented action. */
    struct { csm_state_t s; csm_event_t e; csm_state_t n; csm_action_t a; } seq[] = {
        { CSM_CONTROL_UP,      CSM_EV_CONTROL_OPEN,        CSM_BNB_FORMED,      CSM_ACT_FORM_BNB },
        { CSM_BNB_FORMED,      CSM_EV_INTERRUPT_PUBLISHED, CSM_INTERRUPT_BOUND, CSM_ACT_NONE },
        { CSM_INTERRUPT_BOUND, CSM_EV_BNB_LISTENING,       CSM_INTERPOSED,      CSM_ACT_INSTALL_INTERPOSE },
        { CSM_INTERPOSED,      CSM_EV_INTERPOSE_OK,        CSM_MT_MODE,         CSM_ACT_ENSURE_MT_MODE },
        { CSM_MT_MODE,         CSM_EV_MT_MODE_CONFIRMED,   CSM_HANDLER_UP,      CSM_ACT_INJECT_HANDLER_TRIGGER },
        { CSM_HANDLER_UP,      CSM_EV_HANDLER_SPAWNED,     CSM_STEADY,          CSM_ACT_MARK_STEADY },
        { CSM_STEADY,          CSM_EV_FRAME,               CSM_STEADY,          CSM_ACT_NONE },
    };
    for (unsigned i = 0; i < sizeof(seq)/sizeof(seq[0]); i++) {
        csm_result_t rr = csm_step(seq[i].s, seq[i].e);
        CHECK_EQ(rr.next, seq[i].n);
        CHECK_EQ(rr.action, seq[i].a);
    }
}
TEST_MAIN()
