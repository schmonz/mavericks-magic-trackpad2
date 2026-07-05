#ifndef MT2_USB_REFRAME_H
#define MT2_USB_REFRAME_H
#include <stddef.h>
#include <stdint.h>
#include "voodoo_input.h"   /* VoodooInputEvent — the decode->assembly seam type */

#ifdef __cplusplus
extern "C" {
#endif

/* USB ASSEMBLY: the second half of the un-fused pipeline. Consumes the decoded seam contact-set
 * (a VoodooInputEvent from mt2_usb_decode) and emits the genuine-USB CompactV4 frame: keep only real
 * contacts (drop-lifted), synthesize the MakeTouch/Touching/BreakTouch lifecycle (STATEFUL — remembers
 * the prior frame; call mt2_usb_reframe_reset() at stream start), mt1_encode to report 0x28, then
 * append Apple's 2-byte checksum. MUTATES *frame in place. ts is a synthesized monotonic 22-bit
 * timestamp. Writes to out (needs 4 + 9*N + 2 bytes); sets *outlen. Returns 0, or -1 if out too small.
 * This is USB's private assembly (drop-lifted + g_lc + checksum); it is deliberately NOT unified with
 * BT's mt2_session assembly (settle+liftoff, no checksum) — that reconciliation is a later task. */
int usb_assemble_compactv4(VoodooInputEvent *frame, uint32_t ts,
                           uint8_t *out, size_t out_cap, size_t *outlen);

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
 * too small. Call mt2_usb_reframe_reset() when a new device stream starts.
 *
 * Now a thin COMPOSE of mt2_usb_decode (decode -> VoodooInputEvent seam) + usb_assemble_compactv4
 * (assembly); the reader calls those two halves itself, mirroring BT. Kept as one entry point for
 * existing callers/tests. */
int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen);

/* Clear the per-finger lifecycle history (call at device start). */
void mt2_usb_reframe_reset(void);

/* Physical-button edge for the genuine-USB path. The genuine AppleUSBMultitouchDriver gets the
 * button from a separate button-provider service (handleButton, fed via buttonPublished) that our
 * manual-start lacks — NOT from the MT frame — so the in-frame button bit can never click here.
 * Instead, detect the button edge in the MT2 0x02 report (button = mt2[1] & 0x01) and, on a CHANGE,
 * fill a 16-byte button report for AppleUSBMultitouchDriver::handleButton, which reads the button
 * from report byte[15] (RE'd 2026-06-24). *last holds the previously-seen button (0/1); the caller
 * persists it across reports. Returns 1 if the button changed (out_report filled, *last updated),
 * else 0 (out_report untouched). mt2_len < 2 is treated as no change. */
int mt2_usb_button_edge(const uint8_t *mt2, size_t mt2_len, uint8_t *last, uint8_t *out_report);

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
