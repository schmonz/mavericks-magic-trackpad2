#include "../src/mavericks_lifecycle.h"
#include "test.h"
#include <string.h>

/* Build a frame of n contacts with the given ids, all TS_TOUCHING, size 20. */
static MavericksTouchFrame frame_of(int n, const int *ids) {
    MavericksTouchFrame f; memset(&f, 0, sizeof f);
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
static int slot_of(const MavericksTouchFrame *f, int id) {
    for (int i = 0; i < f->contact_count; i++) if (f->transducers[i].id == id) return i;
    return -1;
}

static void run_tests(void) {
    /* A new contact's FIRST stepped frame becomes TS_START (MakeTouch). */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids[] = {3}; MavericksTouchFrame f = frame_of(1, ids);
      mavericks_lifecycle_step(&lc, &f);
      CHECK_EQ(f.contact_count, 1);
      CHECK_EQ(f.transducers[0].state, TS_START); }

    /* The SAME contact next frame stays TS_TOUCHING (no re-fire), no phantom end. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids[] = {3};
      MavericksTouchFrame f1 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f1);
      MavericksTouchFrame f2 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.contact_count, 1);
      CHECK_EQ(f2.transducers[0].state, TS_TOUCHING); }

    /* When a contact vanishes, step APPENDS a TS_END (BreakTouch) at last-known pos. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids[] = {3};
      MavericksTouchFrame f1 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f1);
      MavericksTouchFrame gone; memset(&gone, 0, sizeof gone);   /* no present contacts */
      mavericks_lifecycle_step(&lc, &gone);
      CHECK_EQ(gone.contact_count, 1);                 /* the synthesized end */
      CHECK_EQ(gone.transducers[0].id, 3);
      CHECK_EQ(gone.transducers[0].state, TS_END);
      CHECK_EQ(gone.transducers[0].currentCoordinates.x, 103);           /* last-known position */
      /* and the end fires only once: a further empty frame yields nothing */
      MavericksTouchFrame gone2; memset(&gone2, 0, sizeof gone2);
      mavericks_lifecycle_step(&lc, &gone2);
      CHECK_EQ(gone2.contact_count, 0); }

    /* Partial lift: one of two contacts lifts -> continuing stays Touching,
       lifted one is appended as TS_END. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids2[] = {1, 4}; MavericksTouchFrame f1 = frame_of(2, ids2); mavericks_lifecycle_step(&lc, &f1);
      int ids1[] = {1};    MavericksTouchFrame f2 = frame_of(1, ids1); mavericks_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.contact_count, 2);
      int s1 = slot_of(&f2, 1), s4 = slot_of(&f2, 4);
      CHECK(s1 >= 0 && s4 >= 0);
      CHECK_EQ(f2.transducers[s1].state, TS_TOUCHING);
      CHECK_EQ(f2.transducers[s4].state, TS_END); }

    /* Two contacts where only the NEW id is started. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids1[] = {1};        MavericksTouchFrame f1 = frame_of(1, ids1); mavericks_lifecycle_step(&lc, &f1);
      int ids2[] = {1, 4};     MavericksTouchFrame f2 = frame_of(2, ids2); mavericks_lifecycle_step(&lc, &f2);
      int s1 = slot_of(&f2, 1), s4 = slot_of(&f2, 4);
      CHECK_EQ(f2.transducers[s1].state, TS_TOUCHING);
      CHECK_EQ(f2.transducers[s4].state, TS_START); }

    /* A contact that lifts and later reappears gets TS_START again. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids[] = {3};
      MavericksTouchFrame f1 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f1);
      MavericksTouchFrame gone; memset(&gone, 0, sizeof gone); mavericks_lifecycle_step(&lc, &gone);
      MavericksTouchFrame f2 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.transducers[0].state, TS_START); }

    /* PRESENCE-BASED: the device reports a transition state on touchdown that our
       decode mislabels TS_END. A present contact must still be promoted by its
       PRESENCE -- first frame -> TS_START, continuing -> TS_TOUCHING -- regardless of
       the incoming per-frame state. (Root cause of "recognizer never sees MakeTouch".) */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      MavericksTouchFrame f; memset(&f, 0, sizeof f);
      f.contact_count = 1; f.transducers[0].id = 3; f.transducers[0].currentCoordinates.pressure = 20;
      f.transducers[0].state = TS_END;                 /* decode mislabeled the touchdown frame */
      mavericks_lifecycle_step(&lc, &f);
      CHECK_EQ(f.transducers[0].state, TS_START);       /* presence wins: first frame = MakeTouch */
      MavericksTouchFrame f2; memset(&f2, 0, sizeof f2);
      f2.contact_count = 1; f2.transducers[0].id = 3; f2.transducers[0].currentCoordinates.pressure = 20;
      f2.transducers[0].state = TS_END;                 /* still mislabeled mid-contact */
      mavericks_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.transducers[0].state, TS_TOUCHING); } /* continuing = Touching, not a lift */

    /* flush: emits TS_END for all still-active ids, then clears. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids2[] = {2, 5}; MavericksTouchFrame f1 = frame_of(2, ids2); mavericks_lifecycle_step(&lc, &f1);
      MavericksTouchFrame out; int any = mavericks_lifecycle_flush(&lc, &out);
      CHECK_EQ(any, 1);
      CHECK_EQ(out.contact_count, 2);
      CHECK_EQ(out.transducers[0].state, TS_END);
      CHECK_EQ(out.transducers[1].state, TS_END);
      /* second flush finds nothing */
      MavericksTouchFrame out2; CHECK_EQ(mavericks_lifecycle_flush(&lc, &out2), 0); }

    /* flush with nothing active returns 0. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      MavericksTouchFrame out; CHECK_EQ(mavericks_lifecycle_flush(&lc, &out), 0); }

    /* reset() forgets history: no pending end, next contact is new. */
    { mavericks_lifecycle_t lc; mavericks_lifecycle_reset(&lc);
      int ids[] = {2}; MavericksTouchFrame f1 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f1);
      mavericks_lifecycle_reset(&lc);
      MavericksTouchFrame out; CHECK_EQ(mavericks_lifecycle_flush(&lc, &out), 0);   /* nothing pending */
      MavericksTouchFrame f2 = frame_of(1, ids); mavericks_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.transducers[0].state, TS_START); }
}
TEST_MAIN()
