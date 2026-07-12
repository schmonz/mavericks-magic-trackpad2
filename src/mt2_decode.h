#ifndef MT2_DECODE_H
#define MT2_DECODE_H
#include "mt2_frame.h"
#include <stdint.h>
#include <stddef.h>

/* Shared Magic Trackpad 2 frame decode. The USB and Bluetooth reports carry the
   SAME N * 9-byte finger records; only the report id and header length differ
   (USB 0x02 / 12 bytes; BT 0x31 / 4 bytes). report_id + header_len select the
   transport framing; the per-finger parse is identical. Returns 0 on success,
   -1 if the report doesn't match / isn't a whole number of finger records. */
int mt2_decode(const uint8_t *report, size_t len, uint8_t report_id,
               size_t header_len, mt2_frame *frame);
#endif
