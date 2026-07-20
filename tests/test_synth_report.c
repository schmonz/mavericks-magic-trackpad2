#include "../src/mavericks_synth_report.h"
#include "test.h"
#include <string.h>

static void run_tests(void) {
    mavericks_synth_regs_t regs;
    mavericks_synth_regs_init(&regs);

    unsigned char out[256];
    unsigned int n = 0;

    /* Geometry report (0xD1 Family ID) is answered from mt2_fill_geometry_report -> n>0, rc OK. */
    int rc = mavericks_synth_answer_report(&regs, 0xD1, out, &n);
    CHECK_EQ(rc, MAVERICKS_SYNTH_OK);
    CHECK(n > 0);

    /* 0xDB (Multitouch ID) is unsupported -> the driver must skip it. */
    rc = mavericks_synth_answer_report(&regs, 0xDB, out, &n);
    CHECK_EQ(rc, MAVERICKS_SYNTH_SKIP);

    /* Non-geometry mode register: SET 0xDC=0x5A, then GET echoes it back (1 byte). */
    mavericks_synth_note_set(&regs, 0xDC, 0x5A);
    n = 0;
    rc = mavericks_synth_answer_report(&regs, 0xDC, out, &n);
    CHECK_EQ(rc, MAVERICKS_SYNTH_OK);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0], 0x5A);

    /* An un-SET non-geometry register echoes 0 (init state), still OK/1 byte. */
    n = 0;
    rc = mavericks_synth_answer_report(&regs, 0xC8, out, &n);
    CHECK_EQ(rc, MAVERICKS_SYNTH_OK);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0], 0x00);
}
TEST_MAIN()
