#ifndef MT2_USB_REFRAME_H
#define MT2_USB_REFRAME_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Genuine-USB byte-level helpers for MT2USBReader's registered transport sink: Apple's
 * handleReport checksum and the handleButton click report. The stream CONDITIONING that
 * used to live here (usb_assemble_compactv4's private lifecycle) moved into the shared
 * mt2_session engine (policy row mt2_policy_default) — see docs/mt-stack/reader-seam-map.md. */

/* Overwrite the last two bytes of buf with Apple's 16-bit additive checksum:
 * low=byte[n-2], high=byte[n-1], where the sum is over bytes[0 .. n-3].
 * Matches AppleUSBMultitouchDriver::validateChecksum (cracked 2026-06-24). */
void mt2_apple_checksum(uint8_t *buf, size_t n);

/* Build the 16-byte AppleUSBMultitouchDriver::handleButton report from a session click mask
 * (0 release / 0x1 primary / 0x2 two-finger secondary). handleButton reads only byte[15];
 * any pressed mask maps to 1 — the secondary-vs-primary decision is Apple's on this path
 * (driven by contact count), exactly as with the raw-bit edge detector this replaces. */
void mt2_usb_click_report(unsigned mask, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
#endif
