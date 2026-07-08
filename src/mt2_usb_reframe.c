#include "mt2_usb_reframe.h"

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

