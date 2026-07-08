/* TEMPORARY parallel-run byte-identity oracle (engine unification, commit 3 of 5).
 *
 * Drives identical MT2 USB report sequences through BOTH assemblies:
 *   OLD: mt2_usb_reframe_reset + mt2_usb_to_compactv4 (private g_lc lifecycle) + the raw-byte
 *        button edge (mt2_usb_button_edge -> byte[15] report)
 *   NEW: mt2_session with mt2_policy_usb + a sink doing mt1_encode + Apple checksum (exactly
 *        MT2USBReader's registered sink) + session click edges -> mt2_usb_click_report
 * and requires byte-identical frame output and identical handleButton byte[15] sequences at
 * every step. Both paths share mt2_usb_decode, so only the post-decode halves are under test.
 * This file is DELETED in the next commit, when the old assembly is removed and
 * test_reader_characterization re-pins the new path against the UNCHANGED goldens. */
#include "test.h"
#include "../src/mt2_session.h"
#include "../src/mt2_usb_decode.h"
#include "../src/mt2_usb_reframe.h"
#include "../src/mt1_encode.h"
#include <string.h>
#include <stdio.h>

#define TS  0x2B2B2u
#define SRC 0x5B

/* ---- NEW path: session(mt2_policy_usb) + encode/checksum sink ---------------------------------- */
typedef struct {
    uint8_t frames[16][64]; size_t flen[16]; int nframes;
    uint8_t clicks[16]; int nclicks;
} cap_t;
static void cap_click(void *c, unsigned mask) {
    cap_t *cap = (cap_t *)c;
    uint8_t rep[16];
    mt2_usb_click_report(mask, rep);
    if (cap->nclicks < 16) cap->clicks[cap->nclicks++] = rep[15];
}
static void cap_feed(void *c, const VoodooInputEvent *f) {
    cap_t *cap = (cap_t *)c;
    if (cap->nframes >= 16) return;
    uint8_t *out = cap->frames[cap->nframes];
    int n = mt1_encode(f, out, 64 - 2, TS);
    if (n < 0) return;
    out[n] = out[n + 1] = 0;
    mt2_apple_checksum(out, (size_t)n + 2);
    cap->flen[cap->nframes++] = (size_t)n + 2;
}
static void cap_arm(void *c, uint32_t ms) { (void)c; (void)ms; }

/* ---- corpus builder: one MT2 USB 0x02 report, up to 2 contacts, explicit button bit ------------ */
/* Layout: 12-byte header (button = byte[1] bit0), then 9-byte records. size==0 marks a lifted slot. */
static size_t mk_report(uint8_t *b, int button,
                        int n1, uint8_t size1, uint8_t id1,
                        int n2, uint8_t size2, uint8_t id2) {
    memset(b, 0, 30);
    b[0] = 0x02; b[1] = (uint8_t)(button ? 0x01 : 0x00);
    for (int i = 2; i < 12; i++) b[i] = (uint8_t)(0xA0 + i);
    size_t len = 12;
    if (n1) { b[12]=0x11; b[13]=0x12; b[14]=0x13; b[15]=0x00;
              b[16]=0x44; b[17]=0x55; b[18]=size1; b[19]=0x77; b[20]=id1; len = 21; }
    if (n1 && n2) { b[21]=0x21; b[22]=0x22; b[23]=0x23; b[24]=0x00;
                    b[25]=0x54; b[26]=0x65; b[27]=size2; b[28]=0x87; b[29]=id2; len = 30; }
    return len;
}

/* Run one sequence through both paths, asserting per-report byte identity + click identity. */
static void run_sequence(const char *tag, const uint8_t reports[][30], const size_t *lens, int n) {
    uint8_t last_btn = 0;                              /* OLD click state */
    uint8_t old_clicks[16]; int old_nclicks = 0;
    mt2_usb_reframe_reset();                           /* OLD lifecycle state */

    mt2_session_t s; memset(&s, 0, sizeof s);          /* NEW state */
    cap_t cap; memset(&cap, 0, sizeof cap);
    mt2_session_sink_t sink = { cap_click, cap_feed, cap_arm, &cap };
    mt2_session_connect(&s, SRC, MT2_STREAMING, &mt2_policy_usb, 0);

    for (int i = 0; i < n; i++) {
        uint8_t old_out[64]; size_t old_len = 0;
        int old_rc = mt2_usb_to_compactv4(reports[i], lens[i], TS, old_out, sizeof old_out, &old_len);
        uint8_t rep[16];
        if (mt2_usb_button_edge(reports[i], lens[i], &last_btn, rep) && old_nclicks < 16)
            old_clicks[old_nclicks++] = rep[15];

        int before = cap.nframes;
        VoodooInputEvent frame;
        if (mt2_usb_decode(reports[i], lens[i], &frame) == 0)
            mt2_session_frame(&s, SRC, &frame, 0, &sink);

        if (old_rc == 0) {
            CHECK_EQ(cap.nframes, before + 1);         /* one frame out per report, both paths */
            if (cap.nframes == before + 1) {
                CHECK_EQ(cap.flen[before], old_len);
                if (cap.flen[before] == old_len &&
                    memcmp(cap.frames[before], old_out, old_len) != 0) {
                    fprintf(stderr, "FAIL %s report %d: frame bytes differ\n", tag, i);
                    test_failures++;
                }
            }
        } else {
            CHECK_EQ(cap.nframes, before);             /* neither path emitted */
        }
    }
    CHECK_EQ(cap.nclicks, old_nclicks);
    for (int i = 0; i < old_nclicks && i < cap.nclicks; i++)
        CHECK_EQ(cap.clicks[i], old_clicks[i]);
    (void)tag;
}

static void run_tests(void) {
    /* 1: tap — make, touch, break, empty tail x2 (covers PASSTHROUGH liftoff + emit-empties). */
    { uint8_t r[5][30]; size_t l[5];
      l[0]=mk_report(r[0],0, 1,0x20,0x06, 0,0,0);
      l[1]=mk_report(r[1],0, 1,0x20,0x06, 0,0,0);
      l[2]=mk_report(r[2],0, 1,0x00,0x06, 0,0,0);     /* size 0 -> lifted -> BreakTouch */
      l[3]=mk_report(r[3],0, 0,0,0, 0,0,0);           /* device empty frame */
      l[4]=mk_report(r[4],0, 0,0,0, 0,0,0);
      run_sequence("tap", r, l, 5); }

    /* 2: physical click during a touch — press edge, hold, release edge, lift. */
    { uint8_t r[5][30]; size_t l[5];
      l[0]=mk_report(r[0],0, 1,0x20,0x06, 0,0,0);
      l[1]=mk_report(r[1],1, 1,0x20,0x06, 0,0,0);     /* button DOWN edge */
      l[2]=mk_report(r[2],1, 1,0x20,0x06, 0,0,0);     /* held: no edge */
      l[3]=mk_report(r[3],0, 1,0x20,0x06, 0,0,0);     /* button UP edge */
      l[4]=mk_report(r[4],0, 1,0x00,0x06, 0,0,0);
      run_sequence("click", r, l, 5); }

    /* 3: two fingers, partial lift, full lift (multi-contact lifecycle carry). */
    { uint8_t r[4][30]; size_t l[4];
      l[0]=mk_report(r[0],0, 1,0x20,0x03, 1,0x22,0x08);
      l[1]=mk_report(r[1],0, 1,0x20,0x03, 1,0x22,0x08);
      l[2]=mk_report(r[2],0, 1,0x20,0x03, 1,0x00,0x08);  /* finger 8 lifts */
      l[3]=mk_report(r[3],0, 1,0x00,0x03, 0,0,0);        /* finger 3 lifts */
      run_sequence("twofinger", r, l, 4); }

    /* 4: two-finger physical click (secondary mask on the NEW path must still map to byte15=1). */
    { uint8_t r[3][30]; size_t l[3];
      l[0]=mk_report(r[0],0, 1,0x20,0x03, 1,0x22,0x08);
      l[1]=mk_report(r[1],1, 1,0x20,0x03, 1,0x22,0x08);  /* press with two down -> mask 0x2 */
      l[2]=mk_report(r[2],0, 1,0x20,0x03, 1,0x22,0x08);  /* release */
      run_sequence("twofinger-click", r, l, 3); }

    /* 5: button edge on a CONTACTLESS report (the emit-empties dimension carries the click). */
    { uint8_t r[3][30]; size_t l[3];
      l[0]=mk_report(r[0],0, 0,0,0, 0,0,0);
      l[1]=mk_report(r[1],1, 0,0,0, 0,0,0);              /* press, no contacts */
      l[2]=mk_report(r[2],0, 0,0,0, 0,0,0);              /* release */
      run_sequence("empty-click", r, l, 3); }
}
TEST_MAIN()
