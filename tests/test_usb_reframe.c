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

/* Build a synthetic 21-byte MT2 USB packet: report id 0x02 + 11 header bytes + one
 * 9-byte contact with recognizable values. Mirrors the live "length 21" single-contact case. */
static size_t make_mt2_packet(uint8_t *b) {
    memset(b, 0, 21);
    b[0] = 0x02;                                  /* report id */
    for (int i = 1; i < 12; i++) b[i] = (uint8_t)(0xA0 + i);   /* arbitrary header bytes */
    for (int i = 0; i < 9; i++)  b[12 + i] = (uint8_t)(0x10 + i); /* the one contact body */
    return 21;
}

static void test_reframe_one_contact(void) {
    uint8_t mt2[21], out[64];
    size_t mn = make_mt2_packet(mt2);
    uint32_t ts = 0x12345;                        /* 22-bit-range timestamp */

    size_t outlen = 0;
    int rc = mt2_usb_to_compactv4(mt2, mn, ts, out, sizeof(out), &outlen);
    CHECK_EQ(rc, 0);

    int N = (int)((mn - 12) / 9);                 /* = 1 */
    /* (a) total length = 6 (0x60 + len + 4-byte CV4 hdr) + 9N + 2 checksum */
    CHECK_EQ(outlen, (size_t)(6 + 9 * N + 2));

    /* (b) framing magic */
    CHECK_EQ(out[0], 0x60);

    /* (c) Apple's frame-count math (parser sees the frame after the 2-byte prefix; len excl checksum):
       flen = outlen - 2(prefix) - 2(checksum); (flen-4)/9 == N */
    int flen = (int)outlen - 2 - 2;
    CHECK_EQ((flen - 4) / 9, N);

    /* (d) timestamp round-trips out of the CV4 header */
    uint32_t ts_back = ((uint32_t)out[3] >> 2) | ((uint32_t)out[4] << 6) | ((uint32_t)out[5] << 14);
    CHECK_EQ(ts_back, ts & 0x3FFFFF);            /* 22-bit */

    /* (e) contact body is bit-identical pass-through (mt2[12..] -> out[6..]) */
    for (int j = 0; j < 9 * N; j++) CHECK_EQ(out[6 + j], mt2[12 + j]);

    /* (f) checksum is valid: recompute over our own output reproduces the trailer */
    uint8_t verify[64];
    memcpy(verify, out, outlen);
    verify[outlen - 2] = verify[outlen - 1] = 0;
    mt2_apple_checksum(verify, outlen);
    CHECK_EQ(verify[outlen - 2], out[outlen - 2]);
    CHECK_EQ(verify[outlen - 1], out[outlen - 1]);
}

static void test_reframe_rejects_bad_input(void) {
    uint8_t mt2[21], out[64]; size_t outlen = 0;
    make_mt2_packet(mt2);
    mt2[0] = 0x01;                                /* not a 0x02 report */
    CHECK_EQ(mt2_usb_to_compactv4(mt2, 21, 0, out, sizeof(out), &outlen), -1);
    /* too-small output buffer */
    mt2[0] = 0x02;
    CHECK_EQ(mt2_usb_to_compactv4(mt2, 21, 0, out, 4, &outlen), -1);
}

static void run_tests(void) {
    test_checksum_trivial();
    test_checksum_live_vector();
    test_reframe_one_contact();
    test_reframe_rejects_bad_input();
}
TEST_MAIN()
