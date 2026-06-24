#ifndef MT2_USB_REFRAME_H
#define MT2_USB_REFRAME_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Overwrite the last two bytes of buf with Apple's 16-bit additive checksum:
 * low=byte[n-2], high=byte[n-1], where the sum is over bytes[0 .. n-3].
 * Matches AppleUSBMultitouchDriver::validateChecksum (cracked 2026-06-24). */
void mt2_apple_checksum(uint8_t *buf, size_t n);

/* Translate one MT2 USB report (report id 0x02, 12-byte header incl. id, then N*9-byte
 * contacts) into the Apple path-binary packet AppleUSBMultitouchDriver::handleReport accepts:
 *   out[0]=0x60 framing, out[1]=length prefix, out[2..5]=CompactV4 4-byte header (type+flags+ts),
 *   out[6..]=the SAME N*9-byte contact bodies (bit-identical), then a 2-byte Apple checksum.
 * ts is a synthesized monotonic 22-bit timestamp supplied by the caller. Writes to out (needs
 * 6 + 9*N + 2 bytes); sets *outlen. Returns 0, or -1 if the input is not a 0x02 report / out too small. */
int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen);

#ifdef __cplusplus
}
#endif
#endif
