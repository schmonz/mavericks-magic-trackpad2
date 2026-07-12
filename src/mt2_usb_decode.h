#ifndef MT2_USB_DECODE_H
#define MT2_USB_DECODE_H
#include "mt2_frame.h"

/* Decode one raw MT2 multitouch report (including the leading report-ID byte,
 * which must be 0x02) into frame. Returns 0 on success, -1 if the buffer is not
 * a recognized trackpad2-USB touch frame. */
int mt2_usb_decode(const uint8_t *report, size_t len, mt2_frame *frame);

#endif
