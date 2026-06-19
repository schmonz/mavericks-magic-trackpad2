#ifndef MT2_PIPELINE_H
#define MT2_PIPELINE_H
#include "touch_model.h"
#include <stdint.h>

typedef enum {
    MT2_STREAMING = 0,    /* USB: keeps streaming frames incl. lifts */
    MT2_EVENT_DRIVEN = 1, /* BT: size-0 contact on lift, then silence */
} mt2_transport_mode_t;

/* 1 if frames may flow (now >= until), else 0. */
int mt2_settle_passed(uint32_t now_ms, uint32_t settle_until_ms);

/* Drop lifted contacts (keep only size > 0) by compacting them out in place;
   ntouches := number of real contacts kept. EVENT_DRIVEN only. From MT2BTReader. */
void mt2_drop_lifted(touch_frame_t *frame);

/* Click edge (ported from MT2Gesture::feedFrame). On a change of button bit,
   writes mask (0 release / 0x1 primary / 0x2 two-finger secondary) and returns 1. */
int mt2_click_changed(unsigned button, int nfingers, unsigned *last_button,
                      unsigned *out_mask);

#define MT2_IDLE_MS  35u   /* silence-detect after a contact */
#define MT2_DECEL_MS 20u   /* spacing between held replays */
#define MT2_LIFTOFF_GAP_MS 10u  /* device-time gap stamped on the trailing absence frame so it
                                   lands after (not coincident with) the BreakTouch frame. Mirrors
                                   the shipping MT2 simulator's ~10ms teardown spacing
                                   (acidanthera/VoodooInput); separates the lift in time so the
                                   recognizer finalizes ONE liftoff, not two (the per-tap phantom
                                   double-click). TUNABLE -- sweep on-device if 10 underseparates. */
#define MT2_SETTLE_MS 0u   /* no settle delay: cold-boot measurement (2026-06-17) found no
                              post-connect burst reaching the pipeline on either transport
                              (interrupt/event endpoints deliver only on touch), and the boot
                              screensaver bug was proven external. Gate seam retained at zero. */

typedef struct {
    touch_frame_t held;  /* last real contact, replayed at zero velocity */
    int step;            /* 0,1 = held replays; 2 = lift; >=3 = done */
} mt2_decel_t;

void mt2_decel_arm(mt2_decel_t *d, const touch_frame_t *held);
void mt2_decel_step(mt2_decel_t *d, touch_frame_t *out,
                    int *out_has_frame, uint32_t *out_rearm_ms);
#endif
