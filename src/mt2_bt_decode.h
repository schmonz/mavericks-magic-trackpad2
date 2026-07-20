#ifndef MT2_BT_DECODE_H
#define MT2_BT_DECODE_H
#include "mavericks_frame.h"

/* Decode one Magic Trackpad 2 *Bluetooth* multitouch report into frame. The caller
 * passes the HID report with the leading 0xA1 HID-over-BT transport byte already
 * stripped, so report[0] must be 0x31 (TRACKPAD2_BT report id). Returns 0 on success,
 * -1 if the buffer is not a recognized trackpad2-BT touch frame. Same 9-byte contact
 * record layout as mt2_usb_decode; only the report id (0x31) and header length (4) differ. */
int mt2_bt_decode(const uint8_t *report, size_t len, MavericksTouchFrame *frame);

#endif
