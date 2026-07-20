#include "mavericks_connect_sm.h"

csm_result_t csm_step(csm_state_t state, csm_event_t event) {
    csm_result_t r; r.next = state; r.action = CSM_ACT_NONE;

    if (event == CSM_EV_DISCONNECT) {              /* rule R4: clean slate from any state */
        r.next = CSM_IDLE; r.action = CSM_ACT_TEARDOWN; return r;
    }

    switch (state) {
    case CSM_IDLE:
        if (event == CSM_EV_CONTROL_ATTACH) { r.next = CSM_CONTROL_UP; r.action = CSM_ACT_WAIT_CONTROL_OPEN; }
        break;
    case CSM_CONTROL_UP:
        if (event == CSM_EV_CONTROL_OPEN) { r.next = CSM_BNB_FORMED; r.action = CSM_ACT_FORM_BNB; }
        break;
    case CSM_BNB_FORMED:
        if (event == CSM_EV_INTERRUPT_PUBLISHED) { r.next = CSM_INTERRUPT_BOUND; r.action = CSM_ACT_NONE; }
        break;
    case CSM_INTERRUPT_BOUND:
        if (event == CSM_EV_BNB_LISTENING) { r.next = CSM_INTERPOSED; r.action = CSM_ACT_INSTALL_INTERPOSE; }
        break;
    case CSM_INTERPOSED:
        if (event == CSM_EV_INTERPOSE_OK) { r.next = CSM_MT_MODE; r.action = CSM_ACT_ENSURE_MT_MODE; }
        break;
    case CSM_MT_MODE:
        if (event == CSM_EV_MT_MODE_CONFIRMED) { r.next = CSM_HANDLER_UP; r.action = CSM_ACT_INJECT_HANDLER_TRIGGER; }
        break;
    case CSM_HANDLER_UP:
        if (event == CSM_EV_HANDLER_SPAWNED) { r.next = CSM_STEADY; r.action = CSM_ACT_MARK_STEADY; }
        break;
    case CSM_STEADY:
        /* CSM_EV_FRAME keeps us steady with no action (idempotent). */
        break;
    }
    return r;
}

static const csm_teardown_step_t kTeardown[] = {
    CSM_TD_RESTORE_DELEGATE, CSM_TD_TERMINATE_BNB, CSM_TD_RELEASE_REFS
};
void csm_teardown_steps(const csm_teardown_step_t **steps, unsigned *count) {
    *steps = kTeardown;
    *count = (unsigned)(sizeof(kTeardown)/sizeof(kTeardown[0]));
}
