#include "test.h"
#include <string.h>
extern "C" {
#include "mt2_session.h"
#include "mt1_encode.h"
}
#include "mt2_voodoo_translate.h"

typedef struct { mt2_frame frames[64]; int n; } rec_t;
static void rec_feed(void *ctx, const mt2_frame *f) {
    rec_t *r = (rec_t *)ctx; if (r->n < 64) r->frames[r->n++] = *f;
}
static void rec_btn(void *ctx, unsigned m)   { (void)ctx; (void)m; }
static void rec_timer(void *ctx, uint32_t ms){ (void)ctx; (void)ms; }

static void run_tests(void) {
    rec_t rec; memset(&rec, 0, sizeof(rec));
    /* positional init in struct field order: post_button_edge, feed_frame, arm_timer, ctx */
    mt2_session_sink_t sink = { rec_btn, rec_feed, rec_timer, &rec };
    mt2_session_t s;
    mt2_session_connect(&s, 1, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);

    /* A VoodooInput satellite's first frame: one active contact, STANDARD fields only. */
    VoodooInputEvent w; memset(&w, 0, sizeof(w));
    w.contact_count = 1;
    w.transducers[0].secondaryId = 3;
    /* isTransducerActive is set for realism but the translator does NOT consult it today:
     * contact activity is inferred from pressure > 0 (mt2_drop_lifted). See translator note. */
    w.transducers[0].isTransducerActive = true;
    w.transducers[0].currentCoordinates.x = 500;
    w.transducers[0].currentCoordinates.y = 500;
    w.transducers[0].currentCoordinates.pressure = 40;

    mt2_frame f = mt2_frame_from_voodoo(&w, 1000, 1000);
    mt2_session_frame(&s, 1, &f, 10, &sink);

    CHECK_EQ(rec.n, 1);   /* exactly one frame: single-contact first touch, no empty/pump frames */
    /* The engine derived MakeTouch on the contact's first frame — the satellite never set it. */
    CHECK_EQ(rec.frames[0].transducers[0].state, TS_START);

    /* And it encodes to a valid MT1 0x28 report with the MakeTouch state nibble (0x30). */
    uint8_t buf[64];
    int nb = mt1_encode(&rec.frames[0], buf, sizeof(buf), 100);
    CHECK(nb == 4 + 9);          /* 4-byte MT1 header + 1x 9-byte contact record = 13 */
    CHECK_EQ(buf[0], 0x28);      /* MT1 report id */
    /* last byte of the first record: high nibble = state (0x30 = MakeTouch), low = fingerID */
    CHECK_EQ(buf[4 + 8] & 0xf0, 0x30);
}
TEST_MAIN()
