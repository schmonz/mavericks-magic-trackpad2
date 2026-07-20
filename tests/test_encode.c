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
    MavericksTouchFrame f = {0};
    f.isPhysicalButtonDown = 1;
    f.contact_count = 1;
    f.transducers[0].id = 9;
    f.transducers[0].state = TS_TOUCHING;
    f.transducers[0].currentCoordinates.x = 752;   /* MT2-space coordinates (from a real frame) */
    f.transducers[0].currentCoordinates.y = 556;
    f.transducers[0].currentCoordinates.pressure = 23;

    uint8_t buf[256];
    int n = mt1_encode(&f, buf, sizeof(buf), 0x25abc /* 18-bit ts */);
    CHECK_EQ(n, 4 + 9);           /* 4-byte header + one 9-byte record */
    CHECK_EQ(buf[0], 0x28);       /* MT1 report id */
    CHECK_EQ(buf[1] & 0x01, 1);   /* button */

    /* Timestamp round-trips through the header bytes the way MultitouchSupport's CompactV4
     * parser reads it: ts = (byte1>>2) | (byte2<<6) | (byte3<<14), 22-bit, full resolution. */
    uint32_t ts = (uint32_t)(buf[1] >> 2) | ((uint32_t)buf[2] << 6) | ((uint32_t)buf[3] << 14);
    CHECK_EQ(ts, 0x25abc & 0x3fffff);

    int id, x, y, state;
    mt1_decode_record(buf + 4, &id, &x, &y, &state);
    CHECK_EQ(id, 9);
    CHECK_EQ(state, 0x40);        /* down -> DRAG (Touching) */

    /* Finger-role: the primary contact (slot 0) must NOT be fingerID 1 (thumb). A thumb chord
     * never reaches the tap-click path (MTTapDragManager::handleTapsForDrag) -- verified via
     * tools/iter_tap.sh. fingerID is the low nibble of t[8]; it must be a valid finger 1..5. */
    int finger_id0 = buf[4 + 8] & 0x0f;
    CHECK_EQ(finger_id0, 2);                       /* slot0 -> index, not thumb */
    CHECK(finger_id0 >= 1 && finger_id0 <= 5);

    /* A contact's FIRST frame is TS_START and must encode to MakeTouch (0x30):
     * the gesture recognizer keys tap-to-click on the MakeTouch->BreakTouch
     * transition; without a distinct first frame the contact reads as already
     * Touching and a tap never commits. (state nibble verified vs the 10.9.5
     * CompactV4 decode: 0x30 -> MTTouchState MakeTouch(3).) */
    f.transducers[0].state = TS_START;
    n = mt1_encode(&f, buf, sizeof(buf), 0x25abc);
    CHECK_EQ(n, 4 + 9);
    mt1_decode_record(buf + 4, &id, &x, &y, &state);
    CHECK_EQ(state, 0x30);        /* first frame -> MakeTouch */

    /* A lifting contact is TS_END and must encode to BreakTouch (0x50): the
     * recognizer needs the MakeTouch->...->BreakTouch transition to commit a tap. */
    f.transducers[0].state = TS_END;
    n = mt1_encode(&f, buf, sizeof(buf), 0x25abc);
    CHECK_EQ(n, 4 + 9);
    mt1_decode_record(buf + 4, &id, &x, &y, &state);
    CHECK_EQ(state, 0x50);        /* lift -> BreakTouch */
    f.transducers[0].state = TS_TOUCHING;  /* restore for later cases */

    /* Native tap-to-click gates on contact DENSITY = size*400/radii, which must exceed
     * 0.75 (RE'd: MTChordCycling::tapHasValidTimingAndStrength reads MTContact+0x5c, fed
     * by t[6]&0x3f). MT2 size units are far smaller than the MT1 recognizer expects, so a
     * tap never reaches the strength threshold. A present (touching) contact must report a
     * firm size for the gate to pass -- independent of the small MT2-derived value. */
    f.transducers[0].state = TS_TOUCHING;
    f.transducers[0].currentCoordinates.pressure = 3;                 /* small MT2-derived size */
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
    MavericksTouchFrame e = {0};
    int m = mt1_encode(&e, buf, sizeof(buf), 0);
    CHECK_EQ(m, 4);
    CHECK_EQ(buf[0], 0x28);

    /* Capacity guard. */
    CHECK_EQ(mt1_encode(&f, buf, 3, 0), -1);
}
TEST_MAIN()
