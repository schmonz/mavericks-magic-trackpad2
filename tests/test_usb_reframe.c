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

/* 21-byte MT2 USB packet with one finger record. size_b6 == 0 marks an empty/lifted slot
   (presence is size-based, mt2_drop_lifted, inside the pipeline). */
static size_t make_mt2_one(uint8_t *b, uint8_t size_b6, uint8_t id_b8) {
    memset(b, 0, 21);
    b[0] = 0x02;
    for (int i = 1; i < 12; i++) b[i] = (uint8_t)(0xA0 + i);
    b[12+0]=0x11; b[12+1]=0x12; b[12+2]=0x13; b[12+3]=0x00;
    b[12+4]=0x44; b[12+5]=0x55; b[12+6]=size_b6; b[12+7]=0x77; b[12+8]=id_b8;
    return 21;
}

static int checksum_ok(const uint8_t *out, size_t n) {
    uint8_t v[64];
    memcpy(v, out, n);
    v[n-2] = v[n-1] = 0;
    mt2_apple_checksum(v, n);
    return v[n-2] == out[n-2] && v[n-1] == out[n-1];
}

static void test_reframe_emits_0x28_frame_with_valid_checksum(void) {
    /* The reframe runs the proven pipeline (decode -> drop_lifted -> lifecycle -> mt1_encode) and
       appends Apple's checksum, producing the SAME 0x28 CompactV4 frame the working MT1/BT path emits
       (which the recognizer drives the cursor from) -- plus the checksum handleReport requires. */
    uint8_t mt2[21], out[64]; size_t outlen = 0;
    mt2_usb_reframe_reset();
    make_mt2_one(mt2, 0x20, 0x06);
    CHECK_EQ(mt2_usb_to_compactv4(mt2, 21, 0x12345, out, sizeof(out), &outlen), 0);
    CHECK_EQ(out[0], 0x28);                        /* CompactV4 PATH frame type, == MT1 report id */
    CHECK_EQ(outlen, (size_t)(4 + 9 + 2));         /* 4-byte hdr + one 9-byte contact + 2 checksum */
    CHECK(checksum_ok(out, outlen));
    /* timestamp round-trips out of the CompactV4 header */
    uint32_t ts_back = ((uint32_t)out[1] >> 2) | ((uint32_t)out[2] << 6) | ((uint32_t)out[3] << 14);
    CHECK_EQ(ts_back, 0x12345u & 0x3FFFFF);
}

static void test_reframe_lifecycle_make_then_drag(void) {
    /* The pipeline's mt2_lifecycle marks a finger's first frame MakeTouch and later frames Touching;
       mt1_encode writes that state in the contact's t[8] high nibble (START 0x30, DRAG 0x40). */
    uint8_t mt2[21], out[64]; size_t outlen = 0;
    mt2_usb_reframe_reset();
    make_mt2_one(mt2, 0x20, 0x06);
    mt2_usb_to_compactv4(mt2, 21, 0, out, sizeof(out), &outlen);
    CHECK_EQ(out[4 + 8] & 0xF0, 0x30);            /* contact 0, first frame -> MakeTouch */
    mt2_usb_to_compactv4(mt2, 21, 0, out, sizeof(out), &outlen);
    CHECK_EQ(out[4 + 8] & 0xF0, 0x40);            /* same finger held -> Touching */
}

static void test_reframe_rejects_non_touch_report(void) {
    uint8_t mt2[21], out[64]; size_t outlen = 0;
    mt2_usb_reframe_reset();
    make_mt2_one(mt2, 0x20, 0x06);
    mt2[0] = 0x01;                                /* not a 0x02 report */
    CHECK_EQ(mt2_usb_to_compactv4(mt2, 21, 0, out, sizeof(out), &outlen), -1);
}

/* --- physical-button edge (genuine-USB handleButton feed) ---------------------------------------
 * The genuine driver never gets the button from the MT frame (its handleButtonState/handleButton are
 * fed by a separate button provider that manual-start lacks). So we detect the button edge in the MT2
 * 0x02 report (byte[1] bit0) and hand AppleUSBMultitouchDriver::handleButton a button report whose
 * byte[15] carries the state (RE'd: handleButton reads only report[0xf]). */

static void test_button_edge_press_fills_byte15(void) {
    uint8_t mt2[21]; memset(mt2, 0, sizeof(mt2)); mt2[0] = 0x02; mt2[1] = 0x01;  /* button DOWN */
    uint8_t last = 0, rep[16];
    CHECK_EQ(mt2_usb_button_edge(mt2, 21, &last, rep), 1);     /* changed -> dispatch */
    CHECK_EQ(rep[15], 0x01);                                   /* button in report byte 15 */
    for (int i = 0; i < 15; i++) CHECK_EQ(rep[i], 0x00);       /* rest zero */
    CHECK_EQ(last, 0x01);                                      /* state advanced */
}

static void test_button_edge_no_change_no_dispatch(void) {
    uint8_t mt2[21]; memset(mt2, 0, sizeof(mt2)); mt2[0] = 0x02; mt2[1] = 0x01;
    uint8_t last = 0x01, rep[16]; memset(rep, 0xEE, sizeof(rep));
    CHECK_EQ(mt2_usb_button_edge(mt2, 21, &last, rep), 0);     /* unchanged -> no dispatch */
    CHECK_EQ(rep[0], 0xEE);                                    /* report left untouched */
}

static void test_button_edge_release_dispatches(void) {
    uint8_t mt2[21]; memset(mt2, 0, sizeof(mt2)); mt2[0] = 0x02; mt2[1] = 0x00;  /* button UP */
    uint8_t last = 0x01, rep[16]; memset(rep, 0xEE, sizeof(rep));
    CHECK_EQ(mt2_usb_button_edge(mt2, 21, &last, rep), 1);     /* 1 -> 0 -> dispatch */
    CHECK_EQ(rep[15], 0x00);
    CHECK_EQ(last, 0x00);
}

static void test_button_edge_masks_bit0_only(void) {
    uint8_t mt2[21]; memset(mt2, 0, sizeof(mt2)); mt2[0] = 0x02; mt2[1] = 0xFE;  /* bit0 clear */
    uint8_t last = 0x01, rep[16];
    CHECK_EQ(mt2_usb_button_edge(mt2, 21, &last, rep), 1);     /* only bit0 counts -> 1->0 */
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
    test_reframe_emits_0x28_frame_with_valid_checksum();
    test_reframe_lifecycle_make_then_drag();
    test_reframe_rejects_non_touch_report();
    test_button_edge_press_fills_byte15();
    test_button_edge_no_change_no_dispatch();
    test_button_edge_release_dispatches();
    test_button_edge_masks_bit0_only();
    test_absence_frame_is_empty_0x28_with_checksum();
}
TEST_MAIN()
