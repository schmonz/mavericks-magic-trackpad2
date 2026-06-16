#include "../src/mt2_bt_decode.h"
#include "test.h"

/* Real frames captured over Bluetooth (DTrace on IOBluetoothHIDDriver::processInterruptData,
 * device primed into multitouch mode via the 0xF1 enable). The on-wire 0xA1 transport byte is
 * stripped; mt2_bt_decode sees report[0]==0x31, a 4-byte header, then N*9-byte records (the
 * SAME record layout as the USB decoder, only the header length differs: 4 vs 12). */
static const uint8_t FRAME_1F[] = {
    0x31,0x48,0x83,0x83,                          /* 4-byte header (button = report[1]&1 = 0) */
    0x7e,0x00,0xe8,0x8f,0xa2,0x64,0x19,0x0f,0x85  /* 1 finger record */
};
static const uint8_t FRAME_CLICK[] = {
    0x31,0x01,0x9f,0x84,                          /* report[1]&1 == 1 -> physical click */
    0x23,0xbf,0x8e,0x8f,0x3d,0x43,0x13,0x41,0x88
};
static const uint8_t FRAME_2F[] = {
    0x31,0x60,0x48,0x84,
    0x05,0x61,0xe8,0x23,0x75,0x51,0x1b,0x07,0xa5,
    0x40,0xde,0xb8,0x23,0x5d,0x4b,0x1a,0x0d,0x89
};

static void run_tests(void) {
    touch_frame_t f = {0};
    int rc = mt2_bt_decode(FRAME_1F, sizeof(FRAME_1F), &f);
    CHECK_EQ(rc, 0);
    CHECK_EQ(f.ntouches, 1);
    CHECK_EQ(f.button, 0);
    /* Golden values hand-derived from the (shared) trackpad2 9-byte record packing. */
    CHECK_EQ(f.touches[0].id, 5);
    CHECK_EQ(f.touches[0].x, 126);
    CHECK_EQ(f.touches[0].y, 192);
    CHECK_EQ(f.touches[0].touch_major, 162);
    CHECK_EQ(f.touches[0].touch_minor, 100);
    CHECK_EQ(f.touches[0].size, 25);
    CHECK(f.touches[0].state == TS_TOUCHING);
    /* Within the MT2 device coordinate range. */
    CHECK(f.touches[0].x > -3678 && f.touches[0].x < 3934);
    CHECK(f.touches[0].y > -2478 && f.touches[0].y < 2587);

    /* Physical click: button bit set, one contact. */
    touch_frame_t fc = {0};
    CHECK_EQ(mt2_bt_decode(FRAME_CLICK, sizeof(FRAME_CLICK), &fc), 0);
    CHECK_EQ(fc.ntouches, 1);
    CHECK_EQ(fc.button, 1);

    /* Two-finger frame: two distinct contacts, no button. */
    touch_frame_t f2 = {0};
    CHECK_EQ(mt2_bt_decode(FRAME_2F, sizeof(FRAME_2F), &f2), 0);
    CHECK_EQ(f2.ntouches, 2);
    CHECK_EQ(f2.button, 0);
    CHECK(f2.touches[0].id != f2.touches[1].id);

    /* A USB-format frame (report id 0x02) must be rejected by the BT decoder. */
    uint8_t usb[] = {0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x31,0x30,0xbb,0x93,
                     0xf0,0x82,0xba,0x8b,0x66,0x58,0x17,0x25,0x09};
    CHECK_EQ(mt2_bt_decode(usb, sizeof(usb), &f), -1);

    /* Too short, and a body that isn't a whole number of records, are rejected. */
    uint8_t shortf[] = {0x31,0x00,0x00};
    CHECK_EQ(mt2_bt_decode(shortf, sizeof(shortf), &f), -1);
    uint8_t partial[] = {0x31,0x00,0x00,0x00, 0x7e,0x00,0xe8};  /* 4 hdr + 3 (not 9) */
    CHECK_EQ(mt2_bt_decode(partial, sizeof(partial), &f), -1);
}
TEST_MAIN()
