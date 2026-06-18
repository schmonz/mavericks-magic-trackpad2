#ifndef MT2_LIFECYCLE_H
#define MT2_LIFECYCLE_H
#include "touch_model.h"
#include <stdint.h>

/* Contact-lifecycle marking across frames.
 *
 * The MT2 device gives us only "touching" vs "lifting" per contact (mt2_decode
 * -> TS_TOUCHING / TS_END); it has no clean "first frame" signal. But the native
 * gesture recognizer keys tap-to-click on the MTTouchState lifecycle
 * MakeTouch -> Touching -> BreakTouch. A contact that appears already Touching
 * reads as "already down" and a tap never commits.
 *
 * mt2_lifecycle tracks which contact ids were present last frame so the FIRST
 * frame of a new contact can be promoted TS_TOUCHING -> TS_START (MakeTouch).
 * State is one bitmask (ids are 0..15, the low nibble of the device record). */
typedef struct {
    uint32_t prev_ids;   /* bitmask of contact ids present in the last marked frame */
} mt2_lifecycle_t;

/* Forget history: the next marked contacts read as new. Call on (re)connect. */
void mt2_lifecycle_reset(mt2_lifecycle_t *lc);

/* In place: promote each TS_TOUCHING contact whose id was absent last frame to
 * TS_START, then record this frame's id set. Contacts that are not TS_TOUCHING
 * are left unchanged (their ids are still recorded as present). */
void mt2_lifecycle_mark(mt2_lifecycle_t *lc, touch_frame_t *frame);

#endif
