#include "../src/mt2_lifecycle.h"
#include "test.h"
#include <string.h>

/* Build a frame of n contacts with the given ids, all TS_TOUCHING, size 20. */
static VoodooInputEvent frame_of(int n, const int *ids) {
    VoodooInputEvent f; memset(&f, 0, sizeof f);
    f.contact_count = n;
    for (int i = 0; i < n; i++) {
        f.transducers[i].id = ids[i];
        f.transducers[i].state = TS_TOUCHING;
        f.transducers[i].currentCoordinates.pressure = 20;
        f.transducers[i].currentCoordinates.x = 100 + ids[i];   /* distinct position per id */
        f.transducers[i].currentCoordinates.y = 200 + ids[i];
    }
    return f;
}

/* Find the slot carrying contact id within a frame, or -1. */
static int slot_of(const VoodooInputEvent *f, int id) {
    for (int i = 0; i < f->contact_count; i++) if (f->transducers[i].id == id) return i;
    return -1;
}

static void run_tests(void) {
    /* A new contact's FIRST stepped frame becomes TS_START (MakeTouch). */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3}; VoodooInputEvent f = frame_of(1, ids);
      mt2_lifecycle_step(&lc, &f);
      CHECK_EQ(f.contact_count, 1);
      CHECK_EQ(f.transducers[0].state, TS_START); }

    /* The SAME contact next frame stays TS_TOUCHING (no re-fire), no phantom end. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      VoodooInputEvent f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      VoodooInputEvent f2 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.contact_count, 1);
      CHECK_EQ(f2.transducers[0].state, TS_TOUCHING); }

    /* When a contact vanishes, step APPENDS a TS_END (BreakTouch) at last-known pos. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      VoodooInputEvent f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      VoodooInputEvent gone; memset(&gone, 0, sizeof gone);   /* no present contacts */
      mt2_lifecycle_step(&lc, &gone);
      CHECK_EQ(gone.contact_count, 1);                 /* the synthesized end */
      CHECK_EQ(gone.transducers[0].id, 3);
      CHECK_EQ(gone.transducers[0].state, TS_END);
      CHECK_EQ(gone.transducers[0].currentCoordinates.x, 103);           /* last-known position */
      /* and the end fires only once: a further empty frame yields nothing */
      VoodooInputEvent gone2; memset(&gone2, 0, sizeof gone2);
      mt2_lifecycle_step(&lc, &gone2);
      CHECK_EQ(gone2.contact_count, 0); }

    /* Partial lift: one of two contacts lifts -> continuing stays Touching,
       lifted one is appended as TS_END. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids2[] = {1, 4}; VoodooInputEvent f1 = frame_of(2, ids2); mt2_lifecycle_step(&lc, &f1);
      int ids1[] = {1};    VoodooInputEvent f2 = frame_of(1, ids1); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.contact_count, 2);
      int s1 = slot_of(&f2, 1), s4 = slot_of(&f2, 4);
      CHECK(s1 >= 0 && s4 >= 0);
      CHECK_EQ(f2.transducers[s1].state, TS_TOUCHING);
      CHECK_EQ(f2.transducers[s4].state, TS_END); }

    /* Two contacts where only the NEW id is started. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids1[] = {1};        VoodooInputEvent f1 = frame_of(1, ids1); mt2_lifecycle_step(&lc, &f1);
      int ids2[] = {1, 4};     VoodooInputEvent f2 = frame_of(2, ids2); mt2_lifecycle_step(&lc, &f2);
      int s1 = slot_of(&f2, 1), s4 = slot_of(&f2, 4);
      CHECK_EQ(f2.transducers[s1].state, TS_TOUCHING);
      CHECK_EQ(f2.transducers[s4].state, TS_START); }

    /* A contact that lifts and later reappears gets TS_START again. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      VoodooInputEvent f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      VoodooInputEvent gone; memset(&gone, 0, sizeof gone); mt2_lifecycle_step(&lc, &gone);
      VoodooInputEvent f2 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.transducers[0].state, TS_START); }

    /* PRESENCE-BASED: the device reports a transition state on touchdown that our
       decode mislabels TS_END. A present contact must still be promoted by its
       PRESENCE -- first frame -> TS_START, continuing -> TS_TOUCHING -- regardless of
       the incoming per-frame state. (Root cause of "recognizer never sees MakeTouch".) */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      VoodooInputEvent f; memset(&f, 0, sizeof f);
      f.contact_count = 1; f.transducers[0].id = 3; f.transducers[0].currentCoordinates.pressure = 20;
      f.transducers[0].state = TS_END;                 /* decode mislabeled the touchdown frame */
      mt2_lifecycle_step(&lc, &f);
      CHECK_EQ(f.transducers[0].state, TS_START);       /* presence wins: first frame = MakeTouch */
      VoodooInputEvent f2; memset(&f2, 0, sizeof f2);
      f2.contact_count = 1; f2.transducers[0].id = 3; f2.transducers[0].currentCoordinates.pressure = 20;
      f2.transducers[0].state = TS_END;                 /* still mislabeled mid-contact */
      mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.transducers[0].state, TS_TOUCHING); } /* continuing = Touching, not a lift */

    /* flush: emits TS_END for all still-active ids, then clears. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids2[] = {2, 5}; VoodooInputEvent f1 = frame_of(2, ids2); mt2_lifecycle_step(&lc, &f1);
      VoodooInputEvent out; int any = mt2_lifecycle_flush(&lc, &out);
      CHECK_EQ(any, 1);
      CHECK_EQ(out.contact_count, 2);
      CHECK_EQ(out.transducers[0].state, TS_END);
      CHECK_EQ(out.transducers[1].state, TS_END);
      /* second flush finds nothing */
      VoodooInputEvent out2; CHECK_EQ(mt2_lifecycle_flush(&lc, &out2), 0); }

    /* flush with nothing active returns 0. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      VoodooInputEvent out; CHECK_EQ(mt2_lifecycle_flush(&lc, &out), 0); }

    /* reset() forgets history: no pending end, next contact is new. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {2}; VoodooInputEvent f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      mt2_lifecycle_reset(&lc);
      VoodooInputEvent out; CHECK_EQ(mt2_lifecycle_flush(&lc, &out), 0);   /* nothing pending */
      VoodooInputEvent f2 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.transducers[0].state, TS_START); }
}
TEST_MAIN()
