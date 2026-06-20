#include "mt2_to_mt1.h"
#include "mt2_bt_decode.h"
#include "mt1_encode.h"
#include "touch_model.h"

int mt2_to_mt1(const uint8_t *in, size_t inlen,
               uint8_t *out, size_t outcap, uint32_t timestamp) {
    if (!in || inlen < 2 || !out || outcap < 2) return 0;

    const uint8_t *report = in;
    size_t rlen = inlen;
    if (in[0] == 0xA1) { report = in + 1; rlen = inlen - 1; }  /* strip transport byte */

    touch_frame_t tf;
    if (mt2_bt_decode(report, rlen, &tf) != 0) return 0;

    out[0] = 0xA1;                                              /* BT-HID input transport byte */
    int n = mt1_encode(&tf, out + 1, outcap - 1, timestamp);
    if (n <= 0) return 0;
    return n + 1;
}
