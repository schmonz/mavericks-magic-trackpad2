#include "../src/conn_trace.h"
#include "test.h"
#include <string.h>

static void run_tests(void) {
    conn_trace_rec_t r;
    r.ts_ms = 1234; r.conn_id = 7;
    r.state = CSM_INTERPOSED; r.event = CSM_EV_INTERPOSE_OK;
    r.chan = (void*)0xab0; r.bnb = (void*)0xcd0; r.deleg = (void*)0xef0; r.ret = 0;

    char buf[160];
    int n = conn_trace_format(buf, sizeof(buf), &r);
    CHECK(n > 0);
    /* Stable, greppable, field-ordered line. */
    CHECK(strcmp(buf,
      "CONNTRACE ts=1234 conn=7 state=INTERPOSED event=INTERPOSE_OK "
      "chan=0xab0 bnb=0xcd0 deleg=0xef0 ret=0") == 0);
}
TEST_MAIN()
