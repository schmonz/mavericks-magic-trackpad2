#include "test.h"
#include "../src/mt2_synth_teardown.h"
#include <string.h>

static char g_log[64];
static void rec(char c) { size_t n = strlen(g_log); if (n < sizeof g_log - 1) { g_log[n] = c; g_log[n+1] = 0; } }
static void op_clear_ready(void *x){ (void)x; rec('R'); }   /* fence FIRST  */
static void op_term_shell(void *x){ (void)x; rec('S'); }
static void op_term_amd(void *x){ (void)x; rec('T'); }
static void op_release_amd(void *x){ (void)x; rec('A'); }
static void op_release_wl(void *x){ (void)x; rec('W'); }    /* workloop LAST */

static void test_order(void) {
    g_log[0] = 0;
    mt2_synth_teardown_ops_t ops = { op_clear_ready, op_term_shell, op_term_amd, op_release_amd, op_release_wl, (void*)1 };
    mt2_synth_teardown_run(&ops);
    CHECK(strcmp(g_log, "RSTAW") == 0);   /* ready-first ... workloop-last, exactly */
}

static void test_optional_ops(void) {   /* a NULL op is skipped, not a crash */
    g_log[0] = 0;
    mt2_synth_teardown_ops_t ops = { op_clear_ready, 0 /*no shell*/, op_term_amd, op_release_amd, op_release_wl, (void*)1 };
    mt2_synth_teardown_run(&ops);
    CHECK(strcmp(g_log, "RTAW") == 0);
}

static void run_tests(void) { test_order(); test_optional_ops(); }
TEST_MAIN()
