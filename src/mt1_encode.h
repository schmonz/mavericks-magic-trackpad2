#ifndef MT1_ENCODE_H
#define MT1_ENCODE_H
#include "touch_model.h"

/* Encode the neutral frame as a Magic Trackpad 1 multitouch input report
 * (report id 0x28, 4-byte header + N*9-byte finger records), scaling
 * coordinates from MT2 space to MT1 space. Writes into buf (capacity cap).
 * Returns the report length, or -1 if it won't fit. */
int mt1_encode(const touch_frame_t *frame, uint8_t *buf, size_t cap);

#endif
