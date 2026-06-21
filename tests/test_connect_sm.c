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

    /* R2 idempotency: re-delivering the consumed event (or an out-of-order one) is a no-op. */
    csm_result_t dup = csm_step(CSM_BNB_FORMED, CSM_EV_CONTROL_OPEN); /* already past OPEN */
    CHECK_EQ(dup.next, CSM_BNB_FORMED);
    CHECK_EQ(dup.action, CSM_ACT_NONE);

    /* R3 single-owner: a second CONTROL_ATTACH while not IDLE does not re-form. */
    csm_result_t reattach = csm_step(CSM_STEADY, CSM_EV_CONTROL_ATTACH);
    CHECK_EQ(reattach.next, CSM_STEADY);
    CHECK_EQ(reattach.action, CSM_ACT_NONE);

    /* Teardown is a fixed, ordered sequence (spike-1 §5): restore delegate -> terminate BNB ->
     * release/null. The kernel runs these in order; the order itself is the invariant. */
    const csm_teardown_step_t *steps; unsigned count;
    csm_teardown_steps(&steps, &count);
    CHECK_EQ(count, 3);
    CHECK_EQ(steps[0], CSM_TD_RESTORE_DELEGATE);
    CHECK_EQ(steps[1], CSM_TD_TERMINATE_BNB);
    CHECK_EQ(steps[2], CSM_TD_RELEASE_REFS);
}
TEST_MAIN()
