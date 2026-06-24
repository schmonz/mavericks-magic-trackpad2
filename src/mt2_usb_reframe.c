#include "mt2_usb_reframe.h"

void mt2_apple_checksum(uint8_t *buf, size_t n) {
    if (n < 3) return;
    unsigned sum = 0;
    for (size_t i = 0; i + 2 < n; i++) sum += buf[i];   /* bytes[0 .. n-3] */
    buf[n - 2] = (uint8_t)(sum & 0xff);
    buf[n - 1] = (uint8_t)((sum >> 8) & 0xff);
}

/* CompactV4 PATH frame type byte (out[0]). RE'd on-device 2026-06-24: MultitouchSupport's frame
 * dispatcher (the function @0x4825) switches on packet[0]; the 0x24..0x29 jump table maps 0x28 ->
 * _MTParse_CompactV4BinaryPath (which calls _MTCompactV4HeaderUnpack + _MTCompactV4BinaryContactUnpack).
 * The earlier 0x60 framing was WRONG: 0x60 is a NOTIFICATION frame type (never carries contacts). The
 * genuine driver's handleReport is NOT 0x60-exclusive (non-0x02 -> validateChecksum -> enqueue), so it
 * forwards a 0x28 frame fine as long as the checksum is valid. */
#define CV4_FRAME_TYPE    0x28
#define MT2_USB_REPORT_ID 0x02
#define MT2_USB_HEADER    12     /* MT2 header incl. report-id byte (mt2_usb_decode.c) */
#define CV4_HEADER        4      /* 4-byte CompactV4 header AT out[0]; contacts at out[4]. No 0x60/len prefix. */
#define CONTACT_BYTES     9

int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen) {
    if (mt2_len < MT2_USB_HEADER || mt2[0] != MT2_USB_REPORT_ID) return -1;
    int n_contacts = (int)((mt2_len - MT2_USB_HEADER) / CONTACT_BYTES);
    size_t total = CV4_HEADER + (size_t)CONTACT_BYTES * n_contacts + 2;
    if (total > out_cap) return -1;

    /* CompactV4 4-byte header at out[0..3], inverse of _MTCompactV4HeaderUnpack:
       [0]=type; [1]= (ts&0x3F)<<2 | flags(bits0,1); [2]=ts>>6; [3]=ts>>14. Contact count = (len-4)/9. */
    out[0] = CV4_FRAME_TYPE;
    out[1] = (uint8_t)(((ts & 0x3F) << 2) | 0x0 /* flags */);
    out[2] = (uint8_t)((ts >> 6)  & 0xFF);
    out[3] = (uint8_t)((ts >> 14) & 0xFF);

    /* contact bodies: bit-identical pass-through */
    for (int k = 0; k < n_contacts; k++) {
        const uint8_t *src = mt2 + MT2_USB_HEADER + CONTACT_BYTES * k;
        uint8_t *dst = out + CV4_HEADER + CONTACT_BYTES * k;
        for (int j = 0; j < CONTACT_BYTES; j++) dst[j] = src[j];
    }

    out[total - 2] = out[total - 1] = 0;
    mt2_apple_checksum(out, total);

    if (outlen) *outlen = total;
    return 0;
}
