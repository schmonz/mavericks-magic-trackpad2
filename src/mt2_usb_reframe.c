#include "mt2_usb_reframe.h"

void mt2_apple_checksum(uint8_t *buf, size_t n) {
    if (n < 3) return;
    unsigned sum = 0;
    for (size_t i = 0; i + 2 < n; i++) sum += buf[i];   /* bytes[0 .. n-3] */
    buf[n - 2] = (uint8_t)(sum & 0xff);
    buf[n - 1] = (uint8_t)((sum >> 8) & 0xff);
}

#define CV4_FRAMING       0x60   /* path-binary framing magic; handleReport gates on this. CONFIRMED. */
#define CV4_FRAME_TYPE    0x01   /* CompactV4 frame/report type (out[2]); PIN ON-DEVICE (Task 0.1/3). */
#define MT2_USB_REPORT_ID 0x02
#define MT2_USB_HEADER    12     /* MT2 header incl. report-id byte (mt2_usb_decode.c) */
#define CV4_PREFIX        2      /* 0x60 + 1 length byte before the CV4 header; PIN ON-DEVICE width. */
#define CV4_HEADER        4
#define CONTACT_BYTES     9

int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen) {
    if (mt2_len < MT2_USB_HEADER || mt2[0] != MT2_USB_REPORT_ID) return -1;
    int n_contacts = (int)((mt2_len - MT2_USB_HEADER) / CONTACT_BYTES);
    size_t total = CV4_PREFIX + CV4_HEADER + (size_t)CONTACT_BYTES * n_contacts + 2;
    if (total > out_cap) return -1;

    out[0] = CV4_FRAMING;
    out[1] = (uint8_t)total;                 /* length prefix (HYPOTHESIS; pinned on-device) */

    /* CompactV4 4-byte header at out[2..5]: type + (ts,flags) packing (inverse of Apple's unpack). */
    out[2] = CV4_FRAME_TYPE;
    out[3] = (uint8_t)(((ts & 0x3F) << 2) | 0x0 /* flags */);
    out[4] = (uint8_t)((ts >> 6)  & 0xFF);
    out[5] = (uint8_t)((ts >> 14) & 0xFF);

    /* contact bodies: bit-identical pass-through */
    for (int k = 0; k < n_contacts; k++) {
        const uint8_t *src = mt2 + MT2_USB_HEADER + CONTACT_BYTES * k;
        uint8_t *dst = out + CV4_PREFIX + CV4_HEADER + CONTACT_BYTES * k;
        for (int j = 0; j < CONTACT_BYTES; j++) dst[j] = src[j];
    }

    out[total - 2] = out[total - 1] = 0;
    mt2_apple_checksum(out, total);

    if (outlen) *outlen = total;
    return 0;
}
