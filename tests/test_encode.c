#include "../src/mt1_encode.h"
#include "test.h"

/* MT1 record decode (mirrors Linux hid-magicmouse.c MAGICTRACKPAD branch),
 * used here to verify what the encoder packed. */
static void mt1_decode_record(const uint8_t *t, int *id, int *x, int *y, int *state) {
    *id = (t[7] << 2 | t[6] >> 6) & 0xf;
    *x = ((int32_t)(((uint32_t)t[1] << 27) | ((uint32_t)t[0] << 19))) >> 19;
    *y = -(((int32_t)(((uint32_t)t[3] << 30) | ((uint32_t)t[2] << 22) |
                      ((uint32_t)t[1] << 14))) >> 19);
    *state = t[8] & 0xf0;
}

static void run_tests(void) {
    touch_frame_t f = {0};
    f.button = 1;
    f.ntouches = 1;
    f.touches[0].id = 9;
    f.touches[0].state = TS_TOUCHING;
    f.touches[0].x = 752;   /* MT2-space coordinates (from a real frame) */
    f.touches[0].y = 556;
    f.touches[0].size = 23;

    uint8_t buf[256];
    int n = mt1_encode(&f, buf, sizeof(buf), 0x25abc /* 18-bit ts */);
    CHECK_EQ(n, 4 + 9);           /* 4-byte header + one 9-byte record */
    CHECK_EQ(buf[0], 0x28);       /* MT1 report id */
    CHECK_EQ(buf[1] & 0x01, 1);   /* button */

    /* Timestamp round-trips through the header where hid-magicmouse.c reads it. */
    uint32_t ts = (uint32_t)(buf[1] >> 6) | ((uint32_t)buf[2] << 2) | ((uint32_t)buf[3] << 10);
    CHECK_EQ(ts, 0x25abc & 0x3ffff);

    int id, x, y, state;
    mt1_decode_record(buf + 4, &id, &x, &y, &state);
    CHECK_EQ(id, 9);
    CHECK_EQ(state, 0x40);        /* down -> DRAG (Touching) */

    /* A contact's FIRST frame is TS_START and must encode to MakeTouch (0x30):
     * the gesture recognizer keys tap-to-click on the MakeTouch->BreakTouch
     * transition; without a distinct first frame the contact reads as already
     * Touching and a tap never commits. (state nibble verified vs the 10.9.5
     * CompactV4 decode: 0x30 -> MTTouchState MakeTouch(3).) */
    f.touches[0].state = TS_START;
    n = mt1_encode(&f, buf, sizeof(buf), 0x25abc);
    CHECK_EQ(n, 4 + 9);
    mt1_decode_record(buf + 4, &id, &x, &y, &state);
    CHECK_EQ(state, 0x30);        /* first frame -> MakeTouch */

    /* A lifting contact is TS_END and must encode to BreakTouch (0x50): the
     * recognizer needs the MakeTouch->...->BreakTouch transition to commit a tap. */
    f.touches[0].state = TS_END;
    n = mt1_encode(&f, buf, sizeof(buf), 0x25abc);
    CHECK_EQ(n, 4 + 9);
    mt1_decode_record(buf + 4, &id, &x, &y, &state);
    CHECK_EQ(state, 0x50);        /* lift -> BreakTouch */
    f.touches[0].state = TS_TOUCHING;  /* restore for later cases */

    /* Native tap-to-click gates on contact DENSITY = size*400/radii, which must exceed
     * 0.75 (RE'd: MTChordCycling::tapHasValidTimingAndStrength reads MTContact+0x5c, fed
     * by t[6]&0x3f). MT2 size units are far smaller than the MT1 recognizer expects, so a
     * tap never reaches the strength threshold. A present (touching) contact must report a
     * firm size for the gate to pass -- independent of the small MT2-derived value. */
    f.touches[0].state = TS_TOUCHING;
    f.touches[0].size = 3;                 /* small MT2-derived size */
    n = mt1_encode(&f, buf, sizeof(buf), 0x25abc);
    CHECK_EQ(n, 4 + 9);
    CHECK_EQ(buf[4 + 6] & 0x3f, 0x3f);     /* boosted to full strength for the density gate */
    /* Expected scaled coords (independently computed): MT2(752,556) -> MT1. */
    CHECK(x >= 625 && x <= 629);  /* ~627 */
    CHECK(y >= 549 && y <= 553);  /* ~551 */
    /* Must land inside MT1 device range. */
    CHECK(x > -2909 && x < 3167);
    CHECK(y > -2456 && y < 2565);

    /* No-touch frame still yields a valid header-only report. */
    touch_frame_t e = {0};
    int m = mt1_encode(&e, buf, sizeof(buf), 0);
    CHECK_EQ(m, 4);
    CHECK_EQ(buf[0], 0x28);

    /* Capacity guard. */
    CHECK_EQ(mt1_encode(&f, buf, 3, 0), -1);
}
TEST_MAIN()
