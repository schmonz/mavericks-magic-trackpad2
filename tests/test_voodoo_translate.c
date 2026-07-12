#include "mt2_voodoo_translate.h"
#include "mt2_coord_range.h"
#include "test.h"
#include <string.h>

static void run_tests(void) {
    mt2_frame f;
    VoodooInputEvent w; memset(&w, 0, sizeof(w));
    w.contact_count = 1;
    w.transducers[0].secondaryId = 7;
    w.transducers[0].isPhysicalButtonDown = true;
    w.transducers[0].currentCoordinates.x = 500;   /* logical max 1000 -> midpoint */
    w.transducers[0].currentCoordinates.y = 0;      /* -> MT2_MIN_Y */
    w.transducers[0].currentCoordinates.pressure = 23;

    f = mt2_frame_from_voodoo(&w, 1000, 1000);
    CHECK_EQ(f.contact_count, 1);
    CHECK_EQ(f.transducers[0].id, 7);
    CHECK_EQ(f.isPhysicalButtonDown, 1);                 /* per-contact button OR'd to frame */
    CHECK_EQ(f.transducers[0].currentCoordinates.x, (MT2_MIN_X + MT2_MAX_X) / 2);  /* 128 */
    CHECK_EQ(f.transducers[0].currentCoordinates.y, MT2_MIN_Y);
    CHECK_EQ(f.transducers[0].currentCoordinates.pressure, 23);
    /* "beyond VoodooInput" fields zeroed — the engine derives/defaults them */
    CHECK_EQ(f.transducers[0].state, TS_NONE);
    CHECK_EQ(f.transducers[0].touch_major, 0);
    CHECK_EQ(f.transducers[0].orientation, 0);

    /* endpoints */
    w.transducers[0].currentCoordinates.x = 0;
    f = mt2_frame_from_voodoo(&w, 1000, 1000);
    CHECK_EQ(f.transducers[0].currentCoordinates.x, MT2_MIN_X);
    w.transducers[0].currentCoordinates.x = 1000;
    f = mt2_frame_from_voodoo(&w, 1000, 1000);
    CHECK_EQ(f.transducers[0].currentCoordinates.x, MT2_MAX_X);

    /* logical-max 0 -> identity fallback (dimension-less satellite still moves) */
    w.transducers[0].currentCoordinates.x = 512;
    f = mt2_frame_from_voodoo(&w, 0, 0);
    CHECK_EQ(f.transducers[0].currentCoordinates.x, 512);

    /* contact_count clamps to the wire array bound (10) */
    w.contact_count = 12;
    f = mt2_frame_from_voodoo(&w, 1000, 1000);
    CHECK_EQ(f.contact_count, 10);
}
TEST_MAIN()
