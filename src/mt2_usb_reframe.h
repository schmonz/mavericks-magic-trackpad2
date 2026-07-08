#ifndef MT2_USB_REFRAME_H
#define MT2_USB_REFRAME_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Genuine-USB byte-level helpers for MT2USBReader's registered transport sink and its
 * post-liftoff absence pump: Apple's handleReport checksum, the handleButton click report,
 * and the zero-contact absence frame. The stream CONDITIONING that used to live here
 * (usb_assemble_compactv4's private lifecycle) moved into the shared mt2_session engine
 * (policy row mt2_policy_usb) — see docs/mt-stack/reader-seam-map.md. */

/* Overwrite the last two bytes of buf with Apple's 16-bit additive checksum:
 * low=byte[n-2], high=byte[n-1], where the sum is over bytes[0 .. n-3].
 * Matches AppleUSBMultitouchDriver::validateChecksum (cracked 2026-06-24). */
void mt2_apple_checksum(uint8_t *buf, size_t n);

/* Build the 16-byte AppleUSBMultitouchDriver::handleButton report from a session click mask
 * (0 release / 0x1 primary / 0x2 two-finger secondary). handleButton reads only byte[15];
 * any pressed mask maps to 1 — the secondary-vs-primary decision is Apple's on this path
 * (driven by contact count), exactly as with the raw-bit edge detector this replaces. */
void mt2_usb_click_report(unsigned mask, uint8_t out[16]);

/* Build a zero-contact CompactV4 absence frame (out[0]=0x28, 4-byte header, no contacts, 2-byte
 * Apple checksum) carrying timestamp ts. The genuine-USB path pumps these after a liftoff so the
 * recognizer's per-frame deferred-commit checks (e.g. MTTapDragManager::sendPendingSecondaryTap)
 * keep running through the double-tap window once the device has gone silent. Writes to out (needs
 * 6 bytes); sets *outlen. Returns 0, or -1 if out too small. */
int mt2_usb_make_absence_frame(uint32_t ts, uint8_t *out, size_t out_cap, size_t *outlen);

#ifdef __cplusplus
}
#endif
#endif
