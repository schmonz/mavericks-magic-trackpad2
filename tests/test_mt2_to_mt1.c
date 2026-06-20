#include "../src/mt2_to_mt1.h"
#include "../src/mt2_bt_decode.h"
#include "../src/mt1_encode.h"
#include "test.h"
#include <string.h>

/* Same 1-finger BT frame the bt_decode test uses (report[0]==0x31, 4-byte header). */
static const uint8_t FRAME_1F[] = {
    0x31,0x48,0x83,0x83,
    0x7e,0x00,0xe8,0x8f,0xa2,0x64,0x19,0x0f,0x85
};

static void run_tests(void) {
    const uint32_t ts = 1234;

    /* Independent reference: decode + encode the bare 0x31 report ourselves. */
    touch_frame_t ref = {0};
    CHECK_EQ(mt2_bt_decode(FRAME_1F, sizeof(FRAME_1F), &ref), 0);
    uint8_t refmt1[256];
    int refn = mt1_encode(&ref, refmt1, sizeof(refmt1), ts);
    CHECK(refn > 0);

    /* Bare 0x31 input (no transport prefix). */
    uint8_t out[256];
    int n = mt2_to_mt1(FRAME_1F, sizeof(FRAME_1F), out, sizeof(out), ts);
    CHECK_EQ(n, refn + 1);          /* 0xA1 + MT1 body */
    CHECK_EQ(out[0], 0xA1);
    CHECK_EQ(memcmp(out + 1, refmt1, refn), 0);

    /* 0xA1-prefixed input must produce the identical result (prefix stripped). */
    uint8_t pref[sizeof(FRAME_1F) + 1];
    pref[0] = 0xA1;
    memcpy(pref + 1, FRAME_1F, sizeof(FRAME_1F));
    uint8_t out2[256];
    int n2 = mt2_to_mt1(pref, sizeof(pref), out2, sizeof(out2), ts);
    CHECK_EQ(n2, n);
    CHECK_EQ(memcmp(out2, out, n), 0);

    /* Garbage / too-short input is dropped, not forwarded. */
    uint8_t junk[1] = { 0x00 };
    CHECK_EQ(mt2_to_mt1(junk, sizeof(junk), out, sizeof(out), ts), 0);
}

TEST_MAIN()
