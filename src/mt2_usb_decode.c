#include "mt2_usb_decode.h"
#include "mt2_decode.h"

/* USB framing: report id 0x02, 12-byte header, then N * 9-byte finger records.
   The shared mt2_decode does the per-finger parse. */
#define MT2_USB_REPORT_ID 0x02
#define MT2_USB_HEADER    12

int mt2_usb_decode(const uint8_t *report, size_t len, VoodooInputEvent *frame) {
    return mt2_decode(report, len, MT2_USB_REPORT_ID, MT2_USB_HEADER, frame);
}
