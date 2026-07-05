#ifndef MT2_LIFECYCLE_H
#define MT2_LIFECYCLE_H
#include "voodoo_input.h"
#include <stdint.h>

/* Contact-lifecycle marking across frames.
 *
 * The MT2 device gives us only "present" contacts per frame (mt2_decode +
 * mt2_drop_lifted), with no clean first-frame or lift signal. But the native
 * gesture recognizer keys tap-to-click on the MTTouchState lifecycle
 * MakeTouch -> Touching -> BreakTouch (verified vs the 10.9.5 CompactV4 decode:
 * the per-touch state nibble IS the MTTouchState; 0x30/0x40/0x50). A contact
 * that only ever reports Touching reads as "already down, then vanished" and a
 * tap never commits.
 *
 * mt2_lifecycle tracks which contact ids were present last frame (and their last
 * position) so we can synthesize the missing transitions:
 *   - a new contact's first frame  -> TS_START (MakeTouch)
 *   - a vanished contact           -> TS_END   (BreakTouch), at last-known position
 * ids are 0..15 (the low nibble of the device record). */
typedef struct {
    uint32_t prev_ids;             /* bitmask of ids present in the last stepped frame */
    VoodooInputTransducer  last[MAX_TOUCHES];    /* last-known touch per id (index = id), for BreakTouch */
} mt2_lifecycle_t;

/* Forget all history: the next contacts read as new, none pending an end.
 * Call on (re)connect. */
void mt2_lifecycle_reset(mt2_lifecycle_t *lc);

/* Advance by one frame of currently-present contacts (post mt2_drop_lifted),
 * in place:
 *   - promote each TS_TOUCHING contact whose id was absent last frame to TS_START;
 *   - APPEND a TS_END contact (last-known position) for each id present last frame
 *     but absent now;
 *   - record this frame's present ids + their touches for next time (ended ids are
 *     dropped from history, so their end is delivered exactly once). */
void mt2_lifecycle_step(mt2_lifecycle_t *lc, VoodooInputEvent *frame);

/* Build a frame of TS_END contacts (last-known position) for every still-active id,
 * then clear history. Returns 1 if any were produced, else 0. Used as a silence
 * watchdog: if the input stream simply stops with no lift frame, deliver the
 * outstanding BreakTouch so the recognizer sees a clean lift. */
int mt2_lifecycle_flush(mt2_lifecycle_t *lc, VoodooInputEvent *out);

#endif
