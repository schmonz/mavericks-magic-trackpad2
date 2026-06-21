#include "mt2_connect_sm.h"

csm_result_t csm_step(csm_state_t state, csm_event_t event) {
    csm_result_t r; r.next = state; r.action = CSM_ACT_NONE;
    if (event == CSM_EV_CONTROL_ATTACH && state == CSM_IDLE) {
        r.next = CSM_CONTROL_UP; r.action = CSM_ACT_WAIT_CONTROL_OPEN;
    }
    return r;
}
