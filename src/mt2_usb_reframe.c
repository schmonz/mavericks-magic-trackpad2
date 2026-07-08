#include "mt2_usb_reframe.h"
#include "mt1_encode.h"       /* mt1_encode -- emits the 0x28 CompactV4 frame */

void mt2_apple_checksum(uint8_t *buf, size_t n) {
    if (n < 3) return;
    unsigned sum = 0;
    for (size_t i = 0; i + 2 < n; i++) sum += buf[i];   /* bytes[0 .. n-3] */
    buf[n - 2] = (uint8_t)(sum & 0xff);
    buf[n - 1] = (uint8_t)((sum >> 8) & 0xff);
}

/* handleButton report from a session click mask. See header. */
void mt2_usb_click_report(unsigned mask, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[15] = mask ? 1 : 0;                     /* handleButton reads only report[0xf] */
}

/* Zero-contact absence frame (post-liftoff pump). See header. */
int mt2_usb_make_absence_frame(uint32_t ts, uint8_t *out, size_t out_cap, size_t *outlen) {
    VoodooInputEvent empty;
    empty.contact_count = 0;
    empty.isPhysicalButtonDown = 0;
    empty.timestamp = 0;
    if (out_cap < 2) return -1;
    int n = mt1_encode(&empty, out, out_cap - 2, ts);   /* 0x28 CompactV4 frame, no contacts */
    if (n < 0) return -1;
    out[n] = out[n + 1] = 0;
    mt2_apple_checksum(out, (size_t)n + 2);
    if (outlen) *outlen = (size_t)n + 2;
    return 0;
}

