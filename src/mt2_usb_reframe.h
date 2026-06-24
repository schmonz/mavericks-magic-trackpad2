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

/* Translate one MT2 USB report (report id 0x02, 12-byte header incl. id, then N*9-byte contacts)
 * into a CompactV4 PATH frame AppleUSBMultitouchDriver::handleReport accepts and MultitouchSupport
 * parses: out[0]=0x28 frame type, out[1..3]=CompactV4 header (flags+22-bit ts), out[4..]=one 9-byte
 * contact per PRESENT finger, then a 2-byte Apple checksum. Presence = contact size>0 (the device's
 * per-frame state bits mislabel touchdown, so we don't trust them — same as mt2_drop_lifted). Per
 * finger (by id) we synthesize the touch lifecycle the recognizer needs — MakeTouch on first frame,
 * Touching while held, BreakTouch (at last position) when it vanishes — which requires remembering the
 * prior frame, so this is STATEFUL. ts is a synthesized monotonic 22-bit timestamp from the caller.
 * Writes to out (needs 4 + 9*N + 2 bytes); sets *outlen. Returns 0, or -1 if not a 0x02 report / out
 * too small. Call mt2_usb_reframe_reset() when a new device stream starts. */
int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen);

/* Clear the per-finger lifecycle history (call at device start). */
void mt2_usb_reframe_reset(void);

#ifdef __cplusplus
}
#endif
#endif
