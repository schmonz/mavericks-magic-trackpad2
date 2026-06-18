#include "../src/mt2_lifecycle.h"
#include "test.h"
#include <string.h>

/* Build a frame of n contacts with the given ids, all TS_TOUCHING. */
static touch_frame_t frame_of(int n, const int *ids) {
    touch_frame_t f; memset(&f, 0, sizeof f);
    f.ntouches = n;
    for (int i = 0; i < n; i++) {
        f.touches[i].id = ids[i];
        f.touches[i].state = TS_TOUCHING;
        f.touches[i].size = 20;
    }
    return f;
}

static void run_tests(void) {
    /* A new contact's FIRST marked frame becomes TS_START (MakeTouch). */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      touch_frame_t f = frame_of(1, ids);
      mt2_lifecycle_mark(&lc, &f);
      CHECK_EQ(f.touches[0].state, TS_START); }

    /* The SAME contact on the next frame stays TS_TOUCHING (no re-fire). */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_mark(&lc, &f1);
      touch_frame_t f2 = frame_of(1, ids); mt2_lifecycle_mark(&lc, &f2);
      CHECK_EQ(f2.touches[0].state, TS_TOUCHING); }

    /* A contact that lifts and later reappears gets TS_START again. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {3};
      touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_mark(&lc, &f1);
      touch_frame_t gone; memset(&gone, 0, sizeof gone);   /* no contacts */
      mt2_lifecycle_mark(&lc, &gone);
      touch_frame_t f2 = frame_of(1, ids); mt2_lifecycle_mark(&lc, &f2);
      CHECK_EQ(f2.touches[0].state, TS_START); }

    /* Two contacts: only the NEW id is started; the continuing one is untouched. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids1[] = {1};            touch_frame_t f1 = frame_of(1, ids1);
      mt2_lifecycle_mark(&lc, &f1);
      int ids2[] = {1, 4};         touch_frame_t f2 = frame_of(2, ids2);
      mt2_lifecycle_mark(&lc, &f2);
      /* slot 0 is id 1 (continuing) -> Touching; slot 1 is id 4 (new) -> Start. */
      CHECK_EQ(f2.touches[0].state, TS_TOUCHING);
      CHECK_EQ(f2.touches[1].state, TS_START); }

    /* reset() forgets history: the next contact is new again. */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      int ids[] = {2};
      touch_frame_t f1 = frame_of(1, ids); mt2_lifecycle_mark(&lc, &f1);
      mt2_lifecycle_reset(&lc);
      touch_frame_t f2 = frame_of(1, ids); mt2_lifecycle_mark(&lc, &f2);
      CHECK_EQ(f2.touches[0].state, TS_START); }

    /* Non-touching contacts are left alone (only TS_TOUCHING is promoted). */
    { mt2_lifecycle_t lc; mt2_lifecycle_reset(&lc);
      touch_frame_t f; memset(&f, 0, sizeof f);
      f.ntouches = 1; f.touches[0].id = 5; f.touches[0].state = TS_END;
      mt2_lifecycle_mark(&lc, &f);
      CHECK_EQ(f.touches[0].state, TS_END); }
}
TEST_MAIN()
