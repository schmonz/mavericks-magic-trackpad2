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

    /* --- inverse translator + round-trip identity (the fidelity gate) --- */
    {
        mt2_frame src; memset(&src, 0, sizeof src);
        src.contact_count = 2;
        src.isPhysicalButtonDown = 1;
        /* contact 0: interior point + real (to-be-dropped) ellipse tail */
        src.transducers[0].id = 5;
        src.transducers[0].currentCoordinates.x = 1234;   /* within [MT2_MIN_X..MT2_MAX_X] */
        src.transducers[0].currentCoordinates.y = -900;   /* within [MT2_MIN_Y..MT2_MAX_Y] */
        src.transducers[0].currentCoordinates.pressure = 41;
        src.transducers[0].currentCoordinates.width = 7;
        src.transducers[0].state = TS_TOUCHING;           /* dropped */
        src.transducers[0].touch_major = 88;              /* dropped */
        src.transducers[0].touch_minor = 44;              /* dropped */
        src.transducers[0].orientation = 3;               /* dropped */
        /* contact 1: axis extremes */
        src.transducers[1].id = 9;
        src.transducers[1].currentCoordinates.x = MT2_MAX_X;
        src.transducers[1].currentCoordinates.y = MT2_MIN_Y;
        src.transducers[1].currentCoordinates.pressure = 255;

        VoodooInputEvent w2 = mt2_voodoo_from_frame(&src, MT2_SPAN_X, MT2_SPAN_Y);
        mt2_frame rt = mt2_frame_from_voodoo(&w2, MT2_SPAN_X, MT2_SPAN_Y);

        CHECK_EQ(rt.contact_count, 2);
        CHECK_EQ(rt.isPhysicalButtonDown, 1);
        /* exact on the wire-representable fields */
        CHECK_EQ(rt.transducers[0].id, 5);
        CHECK_EQ(rt.transducers[0].currentCoordinates.x, 1234);
        CHECK_EQ(rt.transducers[0].currentCoordinates.y, -900);
        CHECK_EQ(rt.transducers[0].currentCoordinates.pressure, 41);
        CHECK_EQ(rt.transducers[0].currentCoordinates.width, 7);
        CHECK_EQ(rt.transducers[1].currentCoordinates.x, MT2_MAX_X);
        CHECK_EQ(rt.transducers[1].currentCoordinates.y, MT2_MIN_Y);
        CHECK_EQ(rt.transducers[1].currentCoordinates.pressure, 255);
        /* the ellipse tail is dropped by the interface (re-derived downstream) */
        CHECK_EQ(rt.transducers[0].state, TS_NONE);
        CHECK_EQ(rt.transducers[0].touch_major, 0);
        CHECK_EQ(rt.transducers[0].touch_minor, 0);
        CHECK_EQ(rt.transducers[0].orientation, 0);
    }
}
TEST_MAIN()
