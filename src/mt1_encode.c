#include "mt1_encode.h"

/* Coordinate ranges from Linux hid-magicmouse.c. */
#define MT2_MIN_X (-3678)
#define MT2_MAX_X  3934
#define MT2_MIN_Y (-2478)
#define MT2_MAX_Y  2587
#define MT1_MIN_X (-2909)
#define MT1_MAX_X  3167
#define MT1_MIN_Y (-2456)
#define MT1_MAX_Y  2565

#define MT1_REPORT_ID 0x28
#define MT1_HEADER    4
#define MT1_RECSZ     9

/* MT1 touch states (Linux): NONE 0x00, START 0x30, DRAG 0x40; mask 0xf0.
 * Confirmed vs the 10.9.5 CompactV4 decode: the state nibble IS the MTTouchState
 * directly (no remap) -- 0x30 -> MakeTouch(3), 0x40 -> Touching(4). The recognizer
 * keys tap-to-click on a distinct MakeTouch first frame, so a contact's first frame
 * must carry START (0x30), not DRAG. */
#define MT1_STATE_NONE  0x00
#define MT1_STATE_START 0x30   /* MakeTouch(3)  -- contact's first frame */
#define MT1_STATE_DRAG  0x40   /* Touching(4)   -- contact held */
#define MT1_STATE_BREAK 0x50   /* BreakTouch(5) -- contact lifting */

/* Firm touch radius (major/minor) for a present contact, to lift density (size*400/radii)
 * above the recognizer's strength gate on both transports. Tuned empirically vs mt_contacts
 * `den` (target comfortably > the BT-passing ~7). Smaller => higher density. */
#define MT1_FIRM_RADIUS 0x20

static int scale(int v, int inMin, int inMax, int outMin, int outMax) {
    long span = inMax - inMin;
    if (span == 0) return outMin;
    long s = (long)(v - inMin) * (outMax - outMin) / span + outMin;
    if (s < outMin) s = outMin;
    if (s > outMax) s = outMax;
    return (int)s;
}

int mt1_encode(const touch_frame_t *frame, uint8_t *buf, size_t cap, uint32_t timestamp) {
    if (!frame || !buf) return -1;
    int nt = frame->ntouches;
    if (nt < 0) nt = 0;
    if (nt > MAX_TOUCHES) nt = MAX_TOUCHES;
    size_t need = MT1_HEADER + (size_t)nt * MT1_RECSZ;
    if (cap < need) return -1;

    /* Device frame timestamp, packed exactly where MultitouchSupport's CompactV4 parser
     * reads it (RE'd from _MTCompactV4HeaderUnpack): a 22-bit value spread as
     *   byte1 = button:1, flag:1, ts[0:5]:6   (ts low 6 bits in bits 2-7)
     *   byte2 = ts[6:13]
     *   byte3 = ts[14:21]
     * so the recognizer recovers ts = (byte1>>2) | (byte2<<6) | (byte3<<14).
     * The old hid-magicmouse layout put ts[0:1] in byte1 bits 6-7 -- 4 positions too high
     * -- so CompactV4 read the value ~16x too large, the gesture-recognizer clock ran
     * ~16x fast, and every tap overran the 250ms tap-duration gate (tap-to-click never
     * committed). This positions the FULL-resolution ms value correctly: the clock now
     * tracks real time AND inter-frame deltas survive (cursor motion intact). */
    uint32_t ts = timestamp & 0x3fffff;
    buf[0] = MT1_REPORT_ID;
    buf[1] = (uint8_t)((frame->button & 0x01) | ((ts & 0x3f) << 2));
    buf[2] = (uint8_t)((ts >> 6) & 0xff);
    buf[3] = (uint8_t)((ts >> 14) & 0xff);

    for (int i = 0; i < nt; i++) {
        const touch_t *in = &frame->touches[i];
        uint8_t *t = buf + MT1_HEADER + i * MT1_RECSZ;

        int x1 = scale(in->x, MT2_MIN_X, MT2_MAX_X, MT1_MIN_X, MT1_MAX_X);
        int y1 = scale(in->y, MT2_MIN_Y, MT2_MAX_Y, MT1_MIN_Y, MT1_MAX_Y);

        unsigned X = (unsigned)x1 & 0x1fff;        /* 13-bit two's complement */
        unsigned V = (unsigned)(-y1) & 0x1fff;     /* y stored negated */

        int id = in->id & 0xf;
        int orient = (in->orientation + 32) & 0x3f;
        int state = (in->state == TS_START)    ? MT1_STATE_START :
                    (in->state == TS_TOUCHING) ? MT1_STATE_DRAG  :
                    (in->state == TS_END)      ? MT1_STATE_BREAK :
                                                 MT1_STATE_NONE;
        /* Finger-role identity: Apple's MTParse_CompactV4BinaryPath reads the LOW nibble
         * of the per-touch state byte (t[8] & 0x0f) as the fingerID, then mt_GetProtocolFingerID
         * returns it. fingerID 0 = "unidentified" -> MultitouchSupport's recognizer classifies
         * the contact as a non-finger and forms no chord (no cursor/scroll/gestures). A valid
         * finger needs fingerID in 1..5 (thumb..pinky). Assign distinct ids per contact slot. */
        int finger_id = (i % 5) + 1;

        /* The firm-strength/radius boost below applies only to a PRESENT (touching) contact.
         * A contact being torn down -- TS_END (BreakTouch, lifting) or TS_NONE (NotTracking,
         * the explicit inactive frame of the liftoff staging) -- keeps its natural (decaying
         * or zeroed) values, so the teardown frames read as a genuine lift, not a fresh firm
         * touch. */
        int present = (in->state == TS_START || in->state == TS_TOUCHING);

        /* Contact size feeds the native tap-to-click strength gate. The recognizer
         * computes density = size*400/radii and requires it > 0.75 (RE'd in
         * MultitouchHID: MTChordCycling::tapHasValidTimingAndStrength reads
         * MTContact+0x5c, sourced from this size field). MT2's size units are far
         * smaller than the MT1 recognizer expects, so passing the raw value through
         * leaves density below threshold and no tap ever commits. Report a firm size
         * (the full 6-bit field) for a present finger so the density gate passes. */
        int sz6 = present ? 0x3f : (in->size & 0x3f);

        /* The same density = size*400/radii gate divides by the touch radii (major/minor).
         * size is force-maxed above, but the RAW MT2 radii were passed through -- and MT2's
         * radii units are LARGER than the MT1 recognizer expects, so the denominator kept
         * density below the strength threshold (measured: BT den~7 PASSES, USB den~3.8 FAILS;
         * USB reports larger radii). Compensate the radii the same way size is compensated:
         * report a firm (small) radius for a present finger so density clears the gate on
         * BOTH transports. TUNABLE. */
        int maj = present ? MT1_FIRM_RADIUS : (in->touch_major & 0xff);
        int min = present ? MT1_FIRM_RADIUS : (in->touch_minor & 0xff);

        t[0] = (uint8_t)(X & 0xff);
        t[1] = (uint8_t)(((X >> 8) & 0x1f) | ((V & 0x07) << 5));
        t[2] = (uint8_t)((V >> 3) & 0xff);
        t[3] = (uint8_t)((V >> 11) & 0x03);
        t[4] = (uint8_t)(maj & 0xff);
        t[5] = (uint8_t)(min & 0xff);
        t[6] = (uint8_t)(sz6 | ((id & 0x03) << 6));
        t[7] = (uint8_t)((orient << 2) | ((id >> 2) & 0x03));
        t[8] = (uint8_t)((state & 0xf0) | (finger_id & 0x0f));
    }
    return (int)need;
}
