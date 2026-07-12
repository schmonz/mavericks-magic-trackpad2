/* Characterization net for the mt2_session SECOND output channel and control paths that the
 * frame-byte net (test_encode / test_reader_characterization) structurally cannot see: the
 * hardware button EDGE (post_button_edge), the settle gate, the single-active-source guard, the
 * full-lift absence+pump, and the timer/watchdog flush.
 *
 * test_session.c already asserts these behaviors by COUNTS. This file is the complementary
 * net: it records EVERY sink callback (post_button_edge / feed_frame / arm_timer) in ORDER into one
 * log and pins the exact call SEQUENCE + mask/contact values. A refactor that reorders emit()'s
 * "post_button_edge before feed_frame", drops the pump absence, changes a click mask, or loses the
 * watchdog arm passes the count-based checks in narrow cases but fails here. These are
 * CHARACTERIZATION tests: they capture what the code does TODAY (a near-miss almost deleted
 * post_button_edge as "dead"); they intentionally fail if the button/timer/liftoff paths change, so
 * the change is a conscious re-pin, not a silent regression. */
#include "../src/mt2_session.h"
#include "test.h"
#include <string.h>

/* ---- ordered event log mock sink ---- */
typedef enum { EV_CLICK, EV_FEED, EV_ARM } ev_kind_t;
typedef struct {
    ev_kind_t kind;
    unsigned  mask;           /* EV_CLICK */
    uint32_t  ms;             /* EV_ARM */
    uint32_t  contact_count;  /* EV_FEED */
} ev_t;
typedef struct { ev_t log[32]; int n; } rec_t;

static void put(rec_t *r, ev_t e) { if (r->n < 32) r->log[r->n] = e; r->n++; }
static void rec_click(void *c, unsigned m)                    { ev_t e={0}; e.kind=EV_CLICK; e.mask=m; put(c,e); }
static void rec_feed (void *c, const mt2_frame *f)     { ev_t e={0}; e.kind=EV_FEED; e.contact_count=f->contact_count; put(c,e); }
static void rec_arm  (void *c, uint32_t ms)                   { ev_t e={0}; e.kind=EV_ARM; e.ms=ms; put(c,e); }
static mt2_session_sink_t mk(rec_t *r){
    mt2_session_sink_t s; s.post_button_edge=rec_click; s.feed_frame=rec_feed; s.arm_timer=rec_arm; s.ctx=r; return s;
}

#define BT  0xB7
#define USB 0x5B

/* Sequence assertion helpers: pin kind + payload at a log index. */
#define EXPECT_CLICK(r,i,m) do { CHECK_EQ((r).log[i].kind, EV_CLICK); CHECK_EQ((r).log[i].mask, (unsigned)(m)); } while(0)
#define EXPECT_FEED(r,i,cc) do { CHECK_EQ((r).log[i].kind, EV_FEED);  CHECK_EQ((r).log[i].contact_count, (uint32_t)(cc)); } while(0)
#define EXPECT_ARM(r,i,t)   do { CHECK_EQ((r).log[i].kind, EV_ARM);   CHECK_EQ((r).log[i].ms, (uint32_t)(t)); } while(0)

static mt2_frame contact(int x, int pressure, int button) {
    mt2_frame f; memset(&f,0,sizeof f);
    f.contact_count = 1;
    f.transducers[0].currentCoordinates.x = x;
    f.transducers[0].currentCoordinates.pressure = pressure;
    f.isPhysicalButtonDown = (uint8_t)button;
    return f;
}

/* 1. Button EDGE -> post_button_edge. One contact, physical button 0 -> 1 -> 1 -> 0 across four frames.
 *    post_button_edge fires ONLY on the two edges (down: 1-finger mask 0x1; up: mask 0x0), NEVER on the
 *    held frame, and emit() posts the click BEFORE the frame it belongs to. feed_frame fires every
 *    frame; the watchdog arms every frame the contact stays down. (Mask values read from
 *    mt2_button_edge in src/mt2_pipeline.c: button-down 1 finger => 0x1, release => 0x0.) */
static void s1_button_edge(void) {
    mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
    mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);

    mt2_frame f0 = contact(50, 20, 0);  mt2_session_frame(&s, USB, &f0, 5000, &k);
    mt2_frame f1 = contact(50, 20, 1);  mt2_session_frame(&s, USB, &f1, 5005, &k);
    mt2_frame f2 = contact(50, 20, 1);  mt2_session_frame(&s, USB, &f2, 5010, &k);
    mt2_frame f3 = contact(50, 20, 0);  mt2_session_frame(&s, USB, &f3, 5015, &k);

    /* frame0 (no edge, 0==0): FEED, ARM  -- no post_button_edge */
    /* frame1 (edge 0->1, 1 finger): CLICK 0x1, FEED, ARM */
    /* frame2 (held 1==1): FEED, ARM      -- no spurious post_button_edge */
    /* frame3 (edge 1->0): CLICK 0x0, FEED, ARM */
    CHECK_EQ(r.n, 10);
    EXPECT_FEED (r,0,1); EXPECT_ARM(r,1,MT2_IDLE_MS);
    EXPECT_CLICK(r,2,0x1u); EXPECT_FEED(r,3,1); EXPECT_ARM(r,4,MT2_IDLE_MS);
    EXPECT_FEED (r,5,1); EXPECT_ARM(r,6,MT2_IDLE_MS);
    EXPECT_CLICK(r,7,0x0u); EXPECT_FEED(r,8,1); EXPECT_ARM(r,9,MT2_IDLE_MS);
}

/* 2. Two-finger physical click -> the SECONDARY (right-click) mask. mt2_button_edge keys the
 *    mask on the contact count at the edge: 2 fingers down => 0x2 (vs 1 finger => 0x1). Pin that
 *    the click posts before the frame and carries 0x2. */
static void s2_twofinger_secondary_mask(void) {
    mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
    mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
    mt2_frame f; memset(&f,0,sizeof f); f.contact_count=2;
    f.transducers[0].currentCoordinates.pressure=20; f.transducers[1].currentCoordinates.pressure=20;
    f.isPhysicalButtonDown=1;
    mt2_session_frame(&s, USB, &f, 5000, &k);
    /* CLICK 0x2 (two-finger secondary), FEED(cc=2), ARM */
    CHECK_EQ(r.n, 3);
    EXPECT_CLICK(r,0,0x2u); EXPECT_FEED(r,1,2); EXPECT_ARM(r,2,MT2_IDLE_MS);
}

/* 3. Settle GATE. With MT2_SETTLE_MS=0 connect never opens a real window, so the gate branch in
 *    mt2_session_frame is characterized directly by seeding settle_until_ms: a frame with
 *    now_ms < settle_until is fully dropped (no post_button_edge, no feed, no arm); a frame at/after it
 *    flows. This pins that the gate blocks EVERYTHING, not just the frame. */
static void s3_settle_gate_blocks(void) {
    mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
    mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
    s.settle_until_ms = 1000;                       /* seed a non-zero gate window */
    mt2_frame f = contact(50, 20, 1);
    mt2_session_frame(&s, USB, &f, 999, &k);        /* before settle: dropped whole */
    CHECK_EQ(r.n, 0);
    mt2_session_frame(&s, USB, &f, 1000, &k);       /* at settle: flows (edge + feed + arm) */
    EXPECT_CLICK(r,0,0x1u); EXPECT_FEED(r,1,1); EXPECT_ARM(r,2,MT2_IDLE_MS);
    CHECK_EQ(r.n, 3);
}

/* 4. Single-active-source GUARD. A frame whose source != the connected active_source is dropped
 *    before the gate/lifecycle; the active source flows. */
static void s4_single_source_guard(void) {
    mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
    mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
    mt2_frame f = contact(50, 20, 0);
    mt2_session_frame(&s, BT,  &f, 5000, &k);  CHECK_EQ(r.n, 0);   /* foreign source: nothing */
    mt2_session_frame(&s, USB, &f, 5000, &k);  CHECK_EQ(r.n, 2);   /* active source: FEED + ARM */
    EXPECT_FEED(r,0,1); EXPECT_ARM(r,1,MT2_IDLE_MS);
}

/* 5. Full-lift ABSENCE + PUMP. Contact down, then a frame where it lifts (pressure 0). drop_lifted
 *    removes it, the lifecycle appends its BreakTouch, and emit_with_liftoff replaces that single
 *    all-BreakTouch frame with TWO empty (contact_count==0) frames -- absence (finalize) + pump --
 *    NOT a BreakTouch frame followed by an absence. No post_button_edge; no watchdog re-arm (cleanly
 *    lifted, prev_ids cleared). */
static void s5_full_lift_absence_pump(void) {
    mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
    mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
    mt2_frame down = contact(70, 20, 0);
    mt2_session_frame(&s, BT, &down, 5000, &k);      /* START + arm */
    r.n = 0;                                         /* isolate the lift */
    mt2_frame lift = contact(70, 0, 0);       /* same contact, pressure 0 */
    mt2_session_frame(&s, BT, &lift, 5010, &k);
    /* exactly two absence frames, no click, no arm */
    CHECK_EQ(r.n, 2);
    EXPECT_FEED(r,0,0);   /* absence: finalizes the path liftoff */
    EXPECT_FEED(r,1,0);   /* pump: lets the recognizer flush the waiting tap-click */
}

/* 6. TIMER / silence WATCHDOG. While a contact is down the frame path arms MT2_IDLE_MS. If the
 *    stream then goes silent (no lift frame), mt2_session_timer flushes the still-down contact as
 *    BreakTouch via emit_with_liftoff -> the same absence + pump pair. Pins that the frame armed
 *    the timer and the timer produced the lift. */
static void s6_timer_watchdog_flush(void) {
    mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
    mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
    mt2_frame f; memset(&f,0,sizeof f);
    f.contact_count=1; f.transducers[0].id=2; f.transducers[0].currentCoordinates.pressure=20;
    f.transducers[0].currentCoordinates.x=88;
    mt2_session_frame(&s, BT, &f, 5000, &k);
    /* down frame: FEED(cc=1) + ARM(IDLE) */
    CHECK_EQ(r.n, 2);
    EXPECT_FEED(r,0,1); EXPECT_ARM(r,1,MT2_IDLE_MS);
    r.n = 0;
    mt2_session_timer(&s, &k);                       /* stream went silent */
    /* watchdog flush of the still-down contact -> absence + pump */
    CHECK_EQ(r.n, 2);
    EXPECT_FEED(r,0,0); EXPECT_FEED(r,1,0);
    r.n = 0;
    mt2_session_timer(&s, &k);                       /* nothing left after flush */
    CHECK_EQ(r.n, 0);
}

static void run_tests(void) {
    s1_button_edge();
    s2_twofinger_secondary_mask();
    s3_settle_gate_blocks();
    s4_single_source_guard();
    s5_full_lift_absence_pump();
    s6_timer_watchdog_flush();
}
TEST_MAIN()
