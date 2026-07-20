#include "mt2_voodoo_translate.h"
#include "mt2_coord_range.h"
#include <string.h>

static int32_t rescale(uint32_t v, uint32_t lmax, int32_t omin, int32_t omax) {
    if (lmax == 0) return (int32_t)v;          /* identity fallback (no advertised dimension);
                                                  assumes two's-complement for v > INT32_MAX,
                                                  which no real satellite coordinate reaches */
    if (v > lmax) v = lmax;                    /* clamp into the advertised range */
    return omin + (int32_t)(((int64_t)v * (omax - omin)) / (int64_t)lmax);
}

MavericksTouchFrame mt2_frame_from_voodoo(const VoodooInputEvent *wire,
                                uint32_t logical_max_x, uint32_t logical_max_y) {
    MavericksTouchFrame f;
    memset(&f, 0, sizeof(f));
    int n = (int)wire->contact_count;   /* UInt8, so 0..255 — no lower-bound guard needed */
    if (n > VOODOO_INPUT_MAX_TRANSDUCERS) n = VOODOO_INPUT_MAX_TRANSDUCERS; /* wire bound (10) */
    if (n > MAVERICKS_MAX_CONTACTS) n = MAVERICKS_MAX_CONTACTS;                         /* internal bound (16) */
    f.contact_count = (uint32_t)n;

    unsigned button = 0;
    for (int i = 0; i < n; i++) {
        const VoodooInputTransducer *t = &wire->transducers[i];
        MavericksTouchContact *c = &f.transducers[i];
        c->id = t->secondaryId;
        c->currentCoordinates.x = rescale(t->currentCoordinates.x, logical_max_x, MT2_MIN_X, MT2_MAX_X);
        c->currentCoordinates.y = rescale(t->currentCoordinates.y, logical_max_y, MT2_MIN_Y, MT2_MAX_Y);
        c->currentCoordinates.pressure = t->currentCoordinates.pressure;
        c->currentCoordinates.width    = t->currentCoordinates.width;
        /* state / touch_major / touch_minor / orientation deliberately left 0:
         * mt2_lifecycle derives state; mt1_encode defaults the radii/orientation */
        if (t->isPhysicalButtonDown) button = 1;
    }
    f.isPhysicalButtonDown = button;
    return f;
}

static uint32_t inv_rescale(int32_t v, uint32_t lmax, int32_t omin, int32_t omax) {
    if (lmax == 0) return (uint32_t)v;         /* identity fallback, mirrors rescale() */
    if (v < omin) v = omin;                    /* clamp into the device range */
    if (v > omax) v = omax;
    return (uint32_t)(((int64_t)(v - omin) * lmax) / (int64_t)(omax - omin));
}

VoodooInputEvent mt2_voodoo_from_frame(const MavericksTouchFrame *f,
                                       uint32_t logical_max_x, uint32_t logical_max_y) {
    VoodooInputEvent w;
    memset(&w, 0, sizeof(w));
    int n = (int)f->contact_count;
    if (n > VOODOO_INPUT_MAX_TRANSDUCERS) n = VOODOO_INPUT_MAX_TRANSDUCERS;  /* wire bound (10) */
    if (n < 0) n = 0;
    w.contact_count = (UInt8)n;
    for (int i = 0; i < n; i++) {
        const MavericksTouchContact *c = &f->transducers[i];
        VoodooInputTransducer *t = &w.transducers[i];
        t->secondaryId = c->id;
        t->isTransducerActive = true;                         /* an emitted contact is active */
        t->isPhysicalButtonDown = f->isPhysicalButtonDown ? true : false;
        t->currentCoordinates.x = inv_rescale(c->currentCoordinates.x, logical_max_x, MT2_MIN_X, MT2_MAX_X);
        t->currentCoordinates.y = inv_rescale(c->currentCoordinates.y, logical_max_y, MT2_MIN_Y, MT2_MAX_Y);
        t->currentCoordinates.pressure = (UInt8)c->currentCoordinates.pressure;
        t->currentCoordinates.width    = (UInt8)c->currentCoordinates.width;
    }
    return w;
}
