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

static int checksum_ok(const uint8_t *out, size_t n) {
    uint8_t v[64];
    memcpy(v, out, n);
    v[n-2] = v[n-1] = 0;
    mt2_apple_checksum(v, n);
    return v[n-2] == out[n-2] && v[n-1] == out[n-1];
}

/* --- session click mask -> handleButton report (byte[15] only) ---------------------------------- */
static void test_click_report_press_maps_any_mask_to_byte15(void) {
    uint8_t rep[16]; memset(rep, 0xEE, sizeof rep);
    mt2_usb_click_report(0x1, rep);
    CHECK_EQ(rep[15], 0x01);
    for (int i = 0; i < 15; i++) CHECK_EQ(rep[i], 0x00);
    mt2_usb_click_report(0x2, rep);           /* secondary mask: still the one physical button */
    CHECK_EQ(rep[15], 0x01);
}
static void test_click_report_release(void) {
    uint8_t rep[16]; memset(rep, 0xEE, sizeof rep);
    mt2_usb_click_report(0, rep);
    CHECK_EQ(rep[15], 0x00);
}

/* --- absence (no-contact) pump frame for the post-liftoff secondary-tap commit ----------------- */
static void test_absence_frame_is_empty_0x28_with_checksum(void) {
    uint8_t out[64]; size_t outlen = 0;
    CHECK_EQ(mt2_usb_make_absence_frame(0x12345, out, sizeof(out), &outlen), 0);
    CHECK_EQ(out[0], 0x28);                          /* CompactV4 PATH frame type */
    CHECK_EQ(outlen, (size_t)(4 + 2));               /* 4-byte hdr + zero contacts + 2 checksum */
    CHECK(checksum_ok(out, outlen));
    uint32_t ts_back = ((uint32_t)out[1] >> 2) | ((uint32_t)out[2] << 6) | ((uint32_t)out[3] << 14);
    CHECK_EQ(ts_back, 0x12345u & 0x3FFFFF);          /* timestamp round-trips */
}

static void run_tests(void) {
    test_checksum_trivial();
    test_checksum_live_vector();
    test_click_report_press_maps_any_mask_to_byte15();
    test_click_report_release();
    test_absence_frame_is_empty_0x28_with_checksum();
}
TEST_MAIN()
