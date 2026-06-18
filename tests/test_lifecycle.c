#include "../src/mt2_lifecycle.h"
#include "test.h"
#include <string.h>

/* Build a frame of n contacts with the given ids, all TS_TOUCHING, size 20. */
static touch_frame_t frame_of(int n, const int *ids) {
    touch_frame_t f; memset(&f, 0, sizeof f);
    f.ntouches = n;
    for (int i = 0; i < n; i++) {
        f.touches[i].id = ids[i];
        f.touches[i].state = TS_TOUCHING;
        f.touches[i].size = 20;
        f.touches[i].x = 100 + ids[i];   /* distinct position per id */
        f.touches[i].y = 200 + ids[i];
    }
    return f;
}

/* Find the slot carrying contact id within a frame, or -1. */
static int slot_of(const touch_frame_t *f, int id) {
    for (int i = 0; i < f->ntouches; i++) if (f->touches[i].id == id) return i;
    return -1;
}

static void run_tests(void) {
    /* A new contact's FIRST stepped frame becomes TS_START (MakeTouch). */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3}; touch_frame_t f = frame_of(1, ids);
      mt2_lifecycle_step(&lc, &f);
      CHECK_EQ(f.ntouches, 1);
      CHECK_EQ(f.touches[0].state, TS_START); }

    /* The SAME contact next frame stays TS_TOUCHING (no re-fire), no phantom end. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      touch_frame_t f2 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.ntouches, 1);
      CHECK_EQ(f2.touches[0].state, TS_TOUCHING); }

    /* When a contact vanishes, step APPENDS a TS_END (BreakTouch) at last-known pos. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      touch_frame_t gone; memset(&gone, 0, sizeof gone);   /* no present contacts */
      mt2_lifecycle_step(&lc, &gone);
      CHECK_EQ(gone.ntouches, 1);                 /* the synthesized end */
      CHECK_EQ(gone.touches[0].id, 3);
      CHECK_EQ(gone.touches[0].state, TS_END);
      CHECK_EQ(gone.touches[0].x, 103);           /* last-known position */
      /* and the end fires only once: a further empty frame yields nothing */
      touch_frame_t gone2; memset(&gone2, 0, sizeof gone2);
      mt2_lifecycle_step(&lc, &gone2);
      CHECK_EQ(gone2.ntouches, 0); }

    /* Partial lift: one of two contacts lifts -> continuing stays Touching,
       lifted one is appended as TS_END. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids2[] = {1, 4}; touch_frame_t f1 = frame_of(2, ids2); mt2_lifecycle_step(&lc, &f1);
      int ids1[] = {1};    touch_frame_t f2 = frame_of(1, ids1); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.ntouches, 2);
      int s1 = slot_of(&f2, 1), s4 = slot_of(&f2, 4);
      CHECK(s1 >= 0 && s4 >= 0);
      CHECK_EQ(f2.touches[s1].state, TS_TOUCHING);
      CHECK_EQ(f2.touches[s4].state, TS_END); }

    /* Two contacts where only the NEW id is started. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids1[] = {1};        touch_frame_t f1 = frame_of(1, ids1); mt2_lifecycle_step(&lc, &f1);
      int ids2[] = {1, 4};     touch_frame_t f2 = frame_of(2, ids2); mt2_lifecycle_step(&lc, &f2);
      int s1 = slot_of(&f2, 1), s4 = slot_of(&f2, 4);
      CHECK_EQ(f2.touches[s1].state, TS_TOUCHING);
      CHECK_EQ(f2.touches[s4].state, TS_START); }

    /* A contact that lifts and later reappears gets TS_START again. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      touch_frame_t gone; memset(&gone, 0, sizeof gone); mt2_lifecycle_step(&lc, &gone);
      touch_frame_t f2 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.touches[0].state, TS_START); }

    /* flush: emits TS_END for all still-active ids, then clears. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids2[] = {2, 5}; touch_frame_t f1 = frame_of(2, ids2); mt2_lifecycle_step(&lc, &f1);
      touch_frame_t out; int any = mt2_lifecycle_flush(&lc, &out);
      CHECK_EQ(any, 1);
      CHECK_EQ(out.ntouches, 2);
      CHECK_EQ(out.touches[0].state, TS_END);
      CHECK_EQ(out.touches[1].state, TS_END);
      /* second flush finds nothing */
      touch_frame_t out2; CHECK_EQ(mt2_lifecycle_flush(&lc, &out2), 0); }

    /* flush with nothing active returns 0. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      touch_frame_t out; CHECK_EQ(mt2_lifecycle_flush(&lc, &out), 0); }

    /* reset() forgets history: no pending end, next contact is new. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {2}; touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f1);
      mt2_lifecycle_reset(&lc);
      touch_frame_t out; CHECK_EQ(mt2_lifecycle_flush(&lc, &out), 0);   /* nothing pending */
      touch_frame_t f2 = frame_of(1, ids); mt2_lifecycle_step(&lc, &f2);
      CHECK_EQ(f2.touches[0].state, TS_START); }
}
TEST_MAIN()
