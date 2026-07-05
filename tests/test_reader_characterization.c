/*
 * Characterization net for the two transport input pipelines (Task 2).
 *
 * Pins the EXACT 0x28 CompactV4 frame bytes each reader emits TODAY, so the upcoming
 * behavior-preserving refactor (unifying BT's MT2Gesture session path and USB's fused
 * mt2_usb_to_compactv4 into one shared engine) cannot silently change what reaches
 * Apple's gesture recognizer. The golden bytes below were CAPTURED from the running
 * current build (not hand-derived) and frozen; a byte-level diff fails the test.
 *
 * It drives the SAME pure calls the readers use (per docs/mt-stack/reader-seam-map.md):
 *   BT : mt2_bt_decode -> mt2_session_frame (settle+lifecycle+liftoff, MT2_EVENT_DRIVEN)
 *        -> sink feed_frame = mt1_encode  (exactly MT2Gesture::submitFrame/sink_feed_frame,
 *        MT2Gesture.cpp:79 + :49). No checksum on the BT feed.
 *   USB: mt2_usb_reframe_reset -> mt2_usb_to_compactv4 (fuses mt2_usb_decode + drop_lifted +
 *        lifecycle + mt1_encode + Apple checksum), exactly MT2USBReader::mt2_usb_handle_report.
 *
 * Real captured raw frames are reused from the existing decode tests:
 *   BT  1F/2F : tests/test_bt_decode.c  (report id 0x31, 4-byte header)
 *   USB 1F/2F : tests/test_decode.c     (report id 0x02, 12-byte header)
 *
 * Timestamps are fixed constants (the readers pass a device/uptime ms; here we control it
 * so the frozen bytes are deterministic). This pins the encoder given a known timestamp.
 */
#include "test.h"
#include "../src/mt2_bt_decode.h"
#include "../src/mt2_session.h"
#include "../src/mt1_encode.h"
#include "../src/mt2_usb_reframe.h"
#include <string.h>
#include <stdio.h>

/* Set to 1, build+run once to DUMP the current bytes, paste into the golden arrays, set back to 0. */
#define CHAR_CAPTURE 0

/* ---- real captured raw MT2 reports (reused from the decode tests) ------------------------------- */
/* BT: 0xA1 transport byte stripped; report[0]==0x31, 4-byte header, then N*9-byte records. */
static const uint8_t BT_1F[] = {
    0x31,0x48,0x83,0x83,
    0x7e,0x00,0xe8,0x8f,0xa2,0x64,0x19,0x0f,0x85
};
static const uint8_t BT_2F[] = {
    0x31,0x60,0x48,0x84,
    0x05,0x61,0xe8,0x23,0x75,0x51,0x1b,0x07,0xa5,
    0x40,0xde,0xb8,0x23,0x5d,0x4b,0x1a,0x0d,0x89
};
/* USB: report id 0x02, 12-byte header, then N*9-byte records. */
static const uint8_t USB_1F[] = {
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x31,0x30,0xbb,0x93,
    0xf0,0x82,0xba,0x8b,0x66,0x58,0x17,0x25,0x09
};
static const uint8_t USB_2F[] = {
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x31,0x90,0x37,0x94,
    0x2f,0xa2,0x92,0x93,0x37,0x58,0x16,0x1c,0x83,
    0x53,0x65,0xa2,0x97,0x5d,0x58,0x16,0x19,0x2b
};

/* Fixed timestamps woven into the CompactV4 header (input we control for determinism). */
#define BT_TS  0x11111u
#define USB_TS 0x22222u

/* ---- BT sink: capture the mt1_encode output MT2Gesture::sink_feed_frame would feed ------------- */
typedef struct { uint8_t buf[64]; int len; int nfeed; } bt_cap_t;
static void bt_click(void *c, unsigned m){ (void)c; (void)m; }
static void bt_arm  (void *c, uint32_t ms){ (void)c; (void)ms; }
static void bt_feed (void *c, const VoodooInputEvent *f){
    bt_cap_t *cap = (bt_cap_t *)c;
    int n = mt1_encode(f, cap->buf, sizeof cap->buf, BT_TS);   /* mirrors MT2Gesture.cpp:61 */
    if (n > 0) { cap->len = n; }
    cap->nfeed++;
}

/* Run one raw BT report through decode -> session -> encode; return the fed 0x28 frame. */
static int bt_pipeline(const uint8_t *raw, size_t rawlen, uint8_t *out, int *nfeed) {
    mt2_session_t s; memset(&s, 0, sizeof s);
    bt_cap_t cap; memset(&cap, 0, sizeof cap);
    mt2_session_sink_t sink = { bt_click, bt_feed, bt_arm, &cap };
    mt2_session_connect(&s, /*source*/0xB7, MT2_EVENT_DRIVEN, /*now*/1000);
    VoodooInputEvent tf; memset(&tf, 0, sizeof tf);
    if (mt2_bt_decode(raw, rawlen, &tf) != 0) return -1;
    mt2_session_frame(&s, 0xB7, &tf, /*now*/1000, &sink);
    memcpy(out, cap.buf, cap.len);
    if (nfeed) *nfeed = cap.nfeed;
    return cap.len;
}

static int usb_pipeline(const uint8_t *raw, size_t rawlen, uint8_t *out) {
    size_t outlen = 0;
    mt2_usb_reframe_reset();
    if (mt2_usb_to_compactv4(raw, rawlen, USB_TS, out, 64, &outlen) != 0) return -1;
    return (int)outlen;
}

static void dump(const char *tag, const uint8_t *b, int n) {
    printf("/* %-8s len=%d */ ", tag, n);
    for (int i = 0; i < n; i++) printf("0x%02x,", b[i]);
    printf("\n");
}

static void check_bytes(const char *tag, const uint8_t *got, int gn,
                        const uint8_t *want, int wn) {
    CHECK_EQ(gn, wn);
    if (gn == wn) {
        int mism = 0;
        for (int i = 0; i < gn; i++) if (got[i] != want[i]) mism++;
        if (mism) { fprintf(stderr, "FAIL %s: %d byte(s) differ from golden\n", tag, mism);
                    test_failures++; dump(tag, got, gn); }
    }
}

/* =========================== GOLDEN (captured from the current build) ===========================
 * Frozen from the CHAR_CAPTURE dump of the unmodified tree. Header byte[1..3] differ per transport
 * because the two paths pass different timestamps (BT_TS vs USB_TS); the checksum tail (last 2 bytes)
 * is present only on USB. Byte offset 4+8 (state|fingerID) reads 0x32 on every first-frame contact:
 * 0x30 MakeTouch | fingerID 2 (index). USB records set t[6] bit7 (size|id<<6) high (0xff vs 0x7f)
 * because the USB-decoded contact ids differ from BT's. */
static const uint8_t GOLD_BT_1F[]  = {
    0x28,0x44,0x44,0x04,0x7f,0x40,0xe8,0x03,0x20,0x20,0x7f,0x81,0x32
};
static const uint8_t GOLD_BT_2F[]  = {
    0x28,0x44,0x44,0x04,0xeb,0xa0,0xe8,0x03,0x20,0x20,0x7f,0x85,0x32,
    0xb5,0x7e,0xb9,0x03,0x20,0x20,0x7f,0x82,0x33
};
static const uint8_t GOLD_USB_1F[] = {
    0x28,0x88,0x88,0x08,0x73,0x22,0xbb,0x03,0x20,0x20,0x7f,0x72,0x32,0xf6,0x03
};
static const uint8_t GOLD_USB_2F[] = {
    0x28,0x88,0x88,0x08,0xd9,0xa1,0x93,0x03,0x20,0x20,0xff,0x80,0x32,
    0x5a,0x44,0xa3,0x03,0x20,0x20,0xff,0x76,0x33,0x6d,0x08
};

static void run_tests(void) {
    uint8_t out[64]; int n, nfeed;

    n = bt_pipeline(BT_1F, sizeof BT_1F, out, &nfeed);
#if CHAR_CAPTURE
    dump("BT_1F", out, n); printf("   (nfeed=%d)\n", nfeed);
#else
    CHECK(n > 0);
    CHECK_EQ(out[0], 0x28);                       /* CompactV4 frame type */
    CHECK_EQ(n, 4 + 9);                           /* header + one contact, no checksum on BT feed */
    CHECK_EQ(out[4 + 8] & 0xf0, 0x30);            /* first frame of the contact -> MakeTouch */
    check_bytes("BT_1F", out, n, GOLD_BT_1F, (int)sizeof GOLD_BT_1F);
#endif

    n = bt_pipeline(BT_2F, sizeof BT_2F, out, &nfeed);
#if CHAR_CAPTURE
    dump("BT_2F", out, n);
#else
    CHECK(n > 0);
    CHECK_EQ(out[0], 0x28);
    CHECK_EQ(n, 4 + 9 + 9);                        /* header + two contacts */
    check_bytes("BT_2F", out, n, GOLD_BT_2F, (int)sizeof GOLD_BT_2F);
#endif

    n = usb_pipeline(USB_1F, sizeof USB_1F, out);
#if CHAR_CAPTURE
    dump("USB_1F", out, n);
#else
    CHECK(n > 0);
    CHECK_EQ(out[0], 0x28);
    CHECK_EQ(n, 4 + 9 + 2);                        /* header + one contact + 2-byte checksum */
    check_bytes("USB_1F", out, n, GOLD_USB_1F, (int)sizeof GOLD_USB_1F);
#endif

    n = usb_pipeline(USB_2F, sizeof USB_2F, out);
#if CHAR_CAPTURE
    dump("USB_2F", out, n);
#else
    CHECK(n > 0);
    CHECK_EQ(out[0], 0x28);
    CHECK_EQ(n, 4 + 9 + 9 + 2);                    /* header + two contacts + checksum */
    check_bytes("USB_2F", out, n, GOLD_USB_2F, (int)sizeof GOLD_USB_2F);
#endif
}
TEST_MAIN()
