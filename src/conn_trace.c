#include "conn_trace.h"
#include <stdio.h>

const char *csm_state_name(csm_state_t s) {
    switch (s) {
    case CSM_IDLE: return "IDLE";
    case CSM_CONTROL_UP: return "CONTROL_UP";
    case CSM_BNB_FORMED: return "BNB_FORMED";
    case CSM_INTERRUPT_BOUND: return "INTERRUPT_BOUND";
    case CSM_INTERPOSED: return "INTERPOSED";
    case CSM_MT_MODE: return "MT_MODE";
    case CSM_HANDLER_UP: return "HANDLER_UP";
    case CSM_STEADY: return "STEADY";
    }
    return "?";
}
const char *csm_event_name(csm_event_t e) {
    switch (e) {
    case CSM_EV_CONTROL_ATTACH: return "CONTROL_ATTACH";
    case CSM_EV_CONTROL_OPEN: return "CONTROL_OPEN";
    case CSM_EV_INTERRUPT_PUBLISHED: return "INTERRUPT_PUBLISHED";
    case CSM_EV_BNB_LISTENING: return "BNB_LISTENING";
    case CSM_EV_INTERPOSE_OK: return "INTERPOSE_OK";
    case CSM_EV_MT_MODE_CONFIRMED: return "MT_MODE_CONFIRMED";
    case CSM_EV_HANDLER_SPAWNED: return "HANDLER_SPAWNED";
    case CSM_EV_FRAME: return "FRAME";
    case CSM_EV_DISCONNECT: return "DISCONNECT";
    }
    return "?";
}
int conn_trace_format(char *buf, size_t n, const conn_trace_rec_t *r) {
    int w = snprintf(buf, n,
        "CONNTRACE ts=%u conn=%d state=%s event=%s chan=%p bnb=%p deleg=%p ret=%d",
        r->ts_ms, r->conn_id, csm_state_name(r->state), csm_event_name(r->event),
        r->chan, r->bnb, r->deleg, r->ret);
    if (w < 0 || (size_t)w >= n) return -1;
    return w;
}
