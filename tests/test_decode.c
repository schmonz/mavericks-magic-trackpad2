#include "../src/mt2_usb_decode.h"
#include "test.h"

/* Real captured frames (captures/raw_{1,2}finger.txt). */
static const uint8_t FRAME_1F[] = {
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x31,0x30,0xbb,0x93,   /* 12-byte header */
    0xf0,0x82,0xba,0x8b,0x66,0x58,0x17,0x25,0x09                   /* 1 finger record */
};
static const uint8_t FRAME_2F[] = {
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x31,0x90,0x37,0x94,
    0x2f,0xa2,0x92,0x93,0x37,0x58,0x16,0x1c,0x83,
    0x53,0x65,0xa2,0x97,0x5d,0x58,0x16,0x19,0x2b
};

static void run_tests(void) {
    VoodooInputEvent f = {0};
    int rc = mt2_usb_decode(FRAME_1F, sizeof(FRAME_1F), &f);
    CHECK_EQ(rc, 0);
    CHECK_EQ(f.contact_count, 1);
    /* Golden values hand-derived from the trackpad2-USB bit packing. */
    CHECK_EQ(f.transducers[0].id, 9);
    CHECK_EQ(f.transducers[0].currentCoordinates.x, 752);
    CHECK_EQ(f.transducers[0].currentCoordinates.y, 556);
    CHECK_EQ(f.transducers[0].currentCoordinates.pressure, 23);
    CHECK(f.transducers[0].state == TS_TOUCHING);
    /* Coordinates must be within the MT2 device range. */
    CHECK(f.transducers[0].currentCoordinates.x > -3678 && f.transducers[0].currentCoordinates.x < 3934);
    CHECK(f.transducers[0].currentCoordinates.y > -2478 && f.transducers[0].currentCoordinates.y < 2587);

    /* Two-finger frame: two distinct contacts. */
    VoodooInputEvent f2 = {0};
    CHECK_EQ(mt2_usb_decode(FRAME_2F, sizeof(FRAME_2F), &f2), 0);
    CHECK_EQ(f2.contact_count, 2);
    CHECK(f2.transducers[0].id != f2.transducers[1].id);

    /* Malformed input is rejected. */
    uint8_t bad[] = {0x02,0x00,0x00};
    CHECK_EQ(mt2_usb_decode(bad, sizeof(bad), &f), -1);
}
TEST_MAIN()
