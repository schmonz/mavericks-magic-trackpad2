#include "mt2_usb_decode.h"

/* Frame layout (matches Linux hid-magicmouse.c TRACKPAD2_USB_REPORT_ID):
 * report id 0x02, 12-byte header, then N * 9-byte finger records. */
#define MT2_REPORT_ID 0x02
#define MT2_HEADER    12
#define MT2_RECSZ     9

int mt2_usb_decode(const uint8_t *report, size_t len, touch_frame_t *frame) {
    if (!report || !frame) return -1;
    if (len < MT2_HEADER || report[0] != MT2_REPORT_ID) return -1;
    if ((len - MT2_HEADER) % MT2_RECSZ != 0) return -1;

    int npoints = (int)((len - MT2_HEADER) / MT2_RECSZ);
    if (npoints > MAX_TOUCHES) npoints = MAX_TOUCHES;

    frame->ntouches = 0;
    frame->button = report[1] & 0x01;
    frame->timestamp = 0;

    for (int i = 0; i < npoints; i++) {
        const uint8_t *t = report + MT2_HEADER + i * MT2_RECSZ;

        /* 13-bit signed coordinates packed across bytes; assemble in uint32
         * then arithmetic-shift a signed reinterpretation for sign extension. */
        uint32_t xb = ((uint32_t)t[1] << 27) | ((uint32_t)t[0] << 19);
        uint32_t yb = ((uint32_t)t[3] << 30) | ((uint32_t)t[2] << 22) |
                      ((uint32_t)t[1] << 14);
        int x = (int32_t)xb >> 19;
        int y = -((int32_t)yb >> 19);

        int statebits = t[3] & 0xC0;   /* 0x80 == finger down */

        touch_t *out = &frame->touches[frame->ntouches++];
        out->id          = t[8] & 0x0f;
        out->state       = (statebits == 0x80) ? TS_TOUCHING : TS_END;
        out->x           = x;
        out->y           = y;
        out->touch_major = t[4];
        out->touch_minor = t[5];
        out->size        = t[6];
        out->orientation = (t[8] >> 5) - 4;
    }
    return 0;
}
