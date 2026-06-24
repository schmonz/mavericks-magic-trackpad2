#include "test.h"
#include "../src/mt2_usb_reframe.h"
#include <string.h>

static void test_checksum_trivial(void) {
    /* 5-byte buffer; sum of bytes[0..2] = 1+2+3 = 6 -> low=0x06 high=0x00 */
    uint8_t b[5] = { 0x01, 0x02, 0x03, 0xff, 0xff };
    mt2_apple_checksum(b, sizeof(b));
    CHECK_EQ(b[3], 0x06);
    CHECK_EQ(b[4], 0x00);
}

static void test_checksum_live_vector(void) {
    /* 21-byte packet whose bytes[0..18] sum to 0x499 (the live dmesg vector). */
    uint8_t b[21];
    memset(b, 0, sizeof(b));
    b[0] = 0x99; b[1] = 0x04;
    unsigned want = 0x499, have = b[0] + b[1];
    for (int i = 2; i < 19 && have < want; i++) {
        unsigned add = (want - have) > 255 ? 255 : (want - have);
        b[i] = (uint8_t)add; have += add;
    }
    CHECK_EQ(have, want);              /* fixture self-check */
    mt2_apple_checksum(b, sizeof(b));
    CHECK_EQ(b[19], 0x99);            /* low byte of 0x499 */
    CHECK_EQ(b[20], 0x04);            /* high byte of 0x499 */
}

static void run_tests(void) {
    test_checksum_trivial();
    test_checksum_live_vector();
}
TEST_MAIN()
