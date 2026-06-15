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

/* MT1 touch states (Linux): NONE 0x00, START 0x30, DRAG 0x40; mask 0xf0. */
#define MT1_STATE_NONE 0x00
#define MT1_STATE_DRAG 0x40

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

    /* 18-bit device timestamp, packed where hid-magicmouse.c reads it:
     * ts = data[1]>>6 | data[2]<<2 | data[3]<<10. button stays in bit 0. */
    uint32_t ts = timestamp & 0x3ffff;
    buf[0] = MT1_REPORT_ID;
    buf[1] = (uint8_t)((frame->button & 0x01) | ((ts & 0x03) << 6));
    buf[2] = (uint8_t)((ts >> 2) & 0xff);
    buf[3] = (uint8_t)((ts >> 10) & 0xff);

    for (int i = 0; i < nt; i++) {
        const touch_t *in = &frame->touches[i];
        uint8_t *t = buf + MT1_HEADER + i * MT1_RECSZ;

        int x1 = scale(in->x, MT2_MIN_X, MT2_MAX_X, MT1_MIN_X, MT1_MAX_X);
        int y1 = scale(in->y, MT2_MIN_Y, MT2_MAX_Y, MT1_MIN_Y, MT1_MAX_Y);

        unsigned X = (unsigned)x1 & 0x1fff;        /* 13-bit two's complement */
        unsigned V = (unsigned)(-y1) & 0x1fff;     /* y stored negated */

        int id = in->id & 0xf;
        int orient = (in->orientation + 32) & 0x3f;
        int state = (in->state == TS_TOUCHING || in->state == TS_START)
                    ? MT1_STATE_DRAG : MT1_STATE_NONE;
        /* Finger-role identity: Apple's MTParse_CompactV4BinaryPath reads the LOW nibble
         * of the per-touch state byte (t[8] & 0x0f) as the fingerID, then mt_GetProtocolFingerID
         * returns it. fingerID 0 = "unidentified" -> MultitouchSupport's recognizer classifies
         * the contact as a non-finger and forms no chord (no cursor/scroll/gestures). A valid
         * finger needs fingerID in 1..5 (thumb..pinky). Assign distinct ids per contact slot. */
        int finger_id = (i % 5) + 1;

        t[0] = (uint8_t)(X & 0xff);
        t[1] = (uint8_t)(((X >> 8) & 0x1f) | ((V & 0x07) << 5));
        t[2] = (uint8_t)((V >> 3) & 0xff);
        t[3] = (uint8_t)((V >> 11) & 0x03);
        t[4] = (uint8_t)(in->touch_major & 0xff);
        t[5] = (uint8_t)(in->touch_minor & 0xff);
        t[6] = (uint8_t)((in->size & 0x3f) | ((id & 0x03) << 6));
        t[7] = (uint8_t)((orient << 2) | ((id >> 2) & 0x03));
        t[8] = (uint8_t)((state & 0xf0) | (finger_id & 0x0f));
    }
    return (int)need;
}
