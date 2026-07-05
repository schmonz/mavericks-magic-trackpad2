#include "mt2_bt_decode.h"
#include "mt2_decode.h"

/* BT framing: report id 0x31, 4-byte header (after the reader strips the 0xA1
   transport byte), then N * 9-byte finger records — identical to USB otherwise.
   The shared mt2_decode does the per-finger parse. */
#define MT2_BT_REPORT_ID 0x31
#define MT2_BT_HEADER    4

int mt2_bt_decode(const uint8_t *report, size_t len, VoodooInputEvent *frame) {
    return mt2_decode(report, len, MT2_BT_REPORT_ID, MT2_BT_HEADER, frame);
}
