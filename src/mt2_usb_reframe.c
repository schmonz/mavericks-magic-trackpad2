#include "mt2_usb_reframe.h"
#include "mt2_usb_decode.h"   /* mt2_usb_decode */
#include "mt2_pipeline.h"     /* mt2_drop_lifted */
#include "mt2_lifecycle.h"    /* mt2_lifecycle_{reset,step} */
#include "mt1_encode.h"       /* mt1_encode -- emits the 0x28 CompactV4 frame */

void mt2_apple_checksum(uint8_t *buf, size_t n) {
    if (n < 3) return;
    unsigned sum = 0;
    for (size_t i = 0; i + 2 < n; i++) sum += buf[i];   /* bytes[0 .. n-3] */
    buf[n - 2] = (uint8_t)(sum & 0xff);
    buf[n - 1] = (uint8_t)((sum >> 8) & 0xff);
}

/* Per-finger lifecycle history for the USB stream (one device). */
static mt2_lifecycle_t g_lc;
static int             g_lc_ready = 0;

void mt2_usb_reframe_reset(void) { mt2_lifecycle_reset(&g_lc); g_lc_ready = 1; }

/* handleButton report from a session click mask. See header. */
void mt2_usb_click_report(unsigned mask, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[15] = mask ? 1 : 0;                     /* handleButton reads only report[0xf] */
}

/* Physical-button edge detector for the genuine-USB handleButton feed. See header. */
int mt2_usb_button_edge(const uint8_t *mt2, size_t mt2_len, uint8_t *last, uint8_t *out_report) {
    if (!mt2 || !last || !out_report || mt2_len < 2) return 0;
    uint8_t btn = mt2[1] & 0x01;
    if (btn == *last) return 0;                 /* no edge -> nothing to dispatch */
    *last = btn;
    for (int i = 0; i < 16; i++) out_report[i] = 0;
    out_report[15] = btn;                       /* handleButton reads the button from report[0xf] */
    return 1;
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

/* USB ASSEMBLY (the second half of the un-fused pipeline): given the decoded seam contact-set
 * (a VoodooInputEvent from mt2_usb_decode — the same seam type the BT path submits), condition it
 * and emit the genuine-USB frame. This is the USB analogue of BT's MT2Gesture session assembly, but
 * kept SEPARATE on purpose: USB uses drop-lifted + a private per-finger lifecycle (g_lc) + Apple's
 * checksum, where BT uses mt2_session's settle+liftoff and no checksum. Reconciling the two assemblies
 * is a later task; here we only expose the seam. mt1_encode emits report 0x28 (the CompactV4 PATH frame
 * type MultitouchSupport parses, 4-byte ts header + firm-density contacts the recognizer drives the
 * cursor from); the genuine USB driver additionally requires a valid checksum (handleReport::
 * validateChecksum), so we append it. NOTE: mutates frame in place (drop-lifted + lifecycle). */
int usb_assemble_compactv4(VoodooInputEvent *frame, uint32_t ts,
                           uint8_t *out, size_t out_cap, size_t *outlen) {
    if (!g_lc_ready) mt2_usb_reframe_reset();

    mt2_drop_lifted(frame);                        /* keep only real (size>0) contacts */
    mt2_lifecycle_step(&g_lc, frame);              /* synthesize MakeTouch/Touching/BreakTouch */

    if (out_cap < 2) return -1;
    int n = mt1_encode(frame, out, out_cap - 2, ts);    /* the 0x28 CompactV4 frame */
    if (n < 0) return -1;

    out[n] = out[n + 1] = 0;
    mt2_apple_checksum(out, (size_t)n + 2);
    if (outlen) *outlen = (size_t)n + 2;
    return 0;
}

/* Genuine-USB feed = decode-to-seam then USB assembly. Retained as the compose of the two halves so
 * existing callers/tests keep one entry point; the reader (MT2USBReader::mt2_usb_handle_report) now
 * calls mt2_usb_decode + usb_assemble_compactv4 in sequence itself, structurally mirroring BT's
 * decode + submitFrame. Non-0x02 reports are rejected (caller passes them through untouched). */
int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen) {
    VoodooInputEvent frame;
    if (mt2_usb_decode(mt2, mt2_len, &frame) != 0) return -1;   /* decode -> VoodooInputEvent (seam) */
    return usb_assemble_compactv4(&frame, ts, out, out_cap, outlen);
}
