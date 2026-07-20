#ifndef CONN_TRACE_H
#define CONN_TRACE_H
#include <stddef.h>
#include "mavericks_connect_sm.h"

typedef struct {
    unsigned ts_ms;
    int conn_id;
    csm_state_t state;
    csm_event_t event;
    const void *chan, *bnb, *deleg;
    int ret;
} conn_trace_rec_t;

const char *csm_state_name(csm_state_t s);   /* "INTERPOSED" etc. */
const char *csm_event_name(csm_event_t e);   /* "INTERPOSE_OK" etc. */

/* Render one canonical CONNTRACE line into buf. Returns strlen written, or -1 if buf too small.
 * Shared by the kernel emitter (via IOLog of this exact string) and the host tests, so emit and
 * re/conn-trace parsing can never drift. */
int conn_trace_format(char *buf, size_t n, const conn_trace_rec_t *r);
#endif
