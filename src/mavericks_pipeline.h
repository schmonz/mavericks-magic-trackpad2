#ifndef MAVERICKS_PIPELINE_H
#define MAVERICKS_PIPELINE_H
/* Pure conditioning primitives for the pipeline's CONDITION stage. See the
   Translate/Condition/Inject banner atop mavericks_session.c for the whole picture. */
#include "mavericks_frame.h"
#include <stdint.h>

typedef enum {
    MAVERICKS_STREAMING = 0,    /* USB: keeps streaming frames incl. lifts */
    MAVERICKS_EVENT_DRIVEN = 1, /* BT: size-0 contact on lift, then silence */
} mavericks_transport_mode_t;

/* 1 if frames may flow (now >= until), else 0. */
int mavericks_settle_passed(uint32_t now_ms, uint32_t settle_until_ms);

/* Drop lifted contacts (keep only size > 0) by compacting them out in place;
   contact_count := number of real contacts kept. EVENT_DRIVEN only. From MT2BTReader. */
void mavericks_drop_lifted(MavericksTouchFrame *frame);

/* Button edge: returns 1 when the device's physical-button bit CHANGED since
   *last_button, writing a faithful mapping of that real hardware button + finger
   count to Apple's click mask (0 release / 0x1 primary / 0x2 two-finger secondary).
   Translation of a hardware button, NOT a synthesized click.
   (Ported from MavericksVoodooInputHost::feedFrame.) */
int mavericks_button_edge(unsigned button, int nfingers, unsigned *last_button,
                      unsigned *out_mask);

#define MAVERICKS_IDLE_MS  35u   /* silence-detect after a contact */
#define MAVERICKS_DECEL_MS 20u   /* spacing between held replays */
#define MAVERICKS_SETTLE_MS 0u   /* no settle delay: cold-boot measurement (2026-06-17) found no
                              post-connect burst reaching the pipeline on either transport
                              (interrupt/event endpoints deliver only on touch), and the boot
                              screensaver bug was proven external. Gate seam retained at zero. */

typedef struct {
    MavericksTouchFrame held;  /* last real contact, replayed at zero velocity */
    int step;            /* 0,1 = held replays; 2 = lift; >=3 = done */
} mavericks_decel_t;

void mavericks_decel_arm(mavericks_decel_t *d, const MavericksTouchFrame *held);
void mavericks_decel_step(mavericks_decel_t *d, MavericksTouchFrame *out,
                    int *out_has_frame, uint32_t *out_rearm_ms);
#endif
