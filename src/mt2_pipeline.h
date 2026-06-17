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

/* Compact out lifted contacts (size<=0) in place; ntouches := real contacts.
   EVENT_DRIVEN only. Ported from MT2BTReader. */
void mt2_drop_lifted(touch_frame_t *frame);
#endif
