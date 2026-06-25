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

/* Genuine-USB feed = the SAME pipeline the working synthetic/BT paths use, then Apple's checksum.
 * mt1_encode already emits report 0x28 — the CompactV4 PATH frame type MultitouchSupport parses — with
 * the 4-byte CompactV4 timestamp header and the contact byte layout the recognizer drives the cursor
 * from (firm density, MakeTouch/Touching/BreakTouch via mt2_lifecycle). The only thing the genuine USB
 * driver needs that submitFrame doesn't is a valid checksum (handleReport::validateChecksum), so we
 * append it. (Earlier hand-built CompactV4 reframe was replaced after it became per-layer whack-a-mole;
 * reusing the proven encoder is the BT-analogous fix.) Non-0x02 reports are rejected (caller passes
 * them through untouched). */
int mt2_usb_to_compactv4(const uint8_t *mt2, size_t mt2_len, uint32_t ts,
                         uint8_t *out, size_t out_cap, size_t *outlen) {
    if (!g_lc_ready) mt2_usb_reframe_reset();

    touch_frame_t frame;
    if (mt2_usb_decode(mt2, mt2_len, &frame) != 0) return -1;
    mt2_drop_lifted(&frame);                       /* keep only real (size>0) contacts */
    mt2_lifecycle_step(&g_lc, &frame);             /* synthesize MakeTouch/Touching/BreakTouch */

    if (out_cap < 2) return -1;
    int n = mt1_encode(&frame, out, out_cap - 2, ts);   /* the 0x28 CompactV4 frame */
    if (n < 0) return -1;

    out[n] = out[n + 1] = 0;
    mt2_apple_checksum(out, (size_t)n + 2);
    if (outlen) *outlen = (size_t)n + 2;
    return 0;
}
