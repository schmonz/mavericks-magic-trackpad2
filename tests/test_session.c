#include "../src/mt2_session.h"
#include "test.h"
#include <string.h>

typedef struct {
    int n_click; unsigned last_mask;
    int n_feed;  mt2_frame last_feed; mt2_frame feeds[8];
    int n_arm;   uint32_t last_arm;
} rec_t;
static void rec_click(void *c, unsigned m){ rec_t*r=c; r->n_click++; r->last_mask=m; }
static void rec_feed (void *c, const mt2_frame *f){ rec_t*r=c; if(r->n_feed<8) r->feeds[r->n_feed]=*f; r->n_feed++; r->last_feed=*f; }
static void rec_arm  (void *c, uint32_t ms){ rec_t*r=c; r->n_arm++; r->last_arm=ms; }
static mt2_session_sink_t mk(rec_t *r){
    mt2_session_sink_t s; s.post_button_edge=rec_click; s.feed_frame=rec_feed; s.arm_timer=rec_arm; s.ctx=r; return s;
}
#define BT  0xB7
#define USB 0x5B

static mt2_frame one(int x){ mt2_frame f; memset(&f,0,sizeof f);
    f.contact_count=1; f.transducers[0].currentCoordinates.pressure=20; f.transducers[0].currentCoordinates.x=x; return f; }

static void run_tests(void) {
    /* settle gate calibrated to 0 (MT2_SETTLE_MS): cold-boot measurement on both
       transports showed NO post-connect burst reaching the pipeline (interrupt/event
       endpoints deliver only on touch), so frames flow immediately from connect with
       no startup delay. The gate mechanism is retained (mt2_settle_passed, covered in
       test_pipeline) as a zero-cost seam should a future device ever need it. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 1000);
      mt2_frame f=one(50);
      mt2_session_frame(&s, BT, &f, 1000, &k); CHECK_EQ(r.n_feed, 1);   /* flows at connect, no gate */
      mt2_session_frame(&s, BT, &f, 1500, &k); CHECK_EQ(r.n_feed, 2); } /* keeps flowing */

    /* single-active guard: non-active source ignored */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
      mt2_frame f=one(50);
      mt2_session_frame(&s, BT,  &f, 9999, &k); CHECK_EQ(r.n_feed, 0);
      mt2_session_frame(&s, USB, &f, 9999, &k); CHECK_EQ(r.n_feed, 1); }

    /* A lone lift frame with no contact ever down produces nothing (no phantom feed). */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
      mt2_frame lift; memset(&lift,0,sizeof lift); lift.contact_count=1; lift.transducers[0].currentCoordinates.pressure=0;
      mt2_session_frame(&s, USB, &lift, 5000, &k);
      CHECK_EQ(r.n_feed, 0); CHECK_EQ(r.n_arm, 0); }

    /* click: two-finger press -> secondary then feed (post-settle) */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
      mt2_frame f; memset(&f,0,sizeof f); f.contact_count=2;
      f.transducers[0].currentCoordinates.pressure=20; f.transducers[1].currentCoordinates.pressure=20; f.isPhysicalButtonDown=1;
      mt2_session_frame(&s, USB, &f, 5000, &k);
      CHECK_EQ(r.n_click, 1); CHECK_EQ(r.last_mask, 0x2u); CHECK_EQ(r.n_feed, 1); }

    /* EVENT_DRIVEN real contact: lift-drop applied, feed, arm IDLE */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
      mt2_frame f; memset(&f,0,sizeof f); f.contact_count=2;
      f.transducers[0].currentCoordinates.pressure=20; f.transducers[0].currentCoordinates.x=70; f.transducers[1].currentCoordinates.pressure=0;   /* one lifted */
      mt2_session_frame(&s, BT, &f, 5000, &k);
      CHECK_EQ(r.n_feed, 1); CHECK_EQ(r.last_feed.contact_count, 1);
      CHECK_EQ(r.n_arm, 1); CHECK_EQ(r.last_arm, MT2_IDLE_MS); }

    /* EVENT_DRIVEN clean (full) lift: emit TWO absence frames (contact_count=0) -- NOT a separate
       BreakTouch frame then an absence (emitting a BreakTouch frame made the native recognizer
       fire handleChordLiftoff TWICE per tap, destabilising the tap-drag cycle). The FIRST absence
       finalizes the path liftoff; the SECOND is a PUMP frame -- the recognizer only re-checks its
       armed "waiting click" timeout when a frame arrives, and with no frame after the lift it never
       flushes (the click batches onto the NEXT tap as a ~6ms phantom double, or the last tap drops).
       The 2nd absence gives it that cycle, so the tap-click flushes cleanly via
       MTTapDragManager::sendWaitingClickAtHalfTimeout instead of the erratic handleTapsForDrag
       drag-cycle path -- one clean click per tap (verified hands-free: tools/trace_btnstack.d shows
       16 posts ALL via sendWaitingClickAtHalfTimeout for 8 taps; tools/tap_clicks.sh shows phantom
       0). Two ABSENCE frames do NOT double handleChordLiftoff (only the first absence is a lift
       transition; the 2nd is "still no contact"). Arms NO watchdog (cleanly lifted). */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
      mt2_frame real=one(70); mt2_session_frame(&s, BT, &real, 5000, &k);
      rec_t r2={0}; k=mk(&r2);
      mt2_frame lift; memset(&lift,0,sizeof lift); lift.contact_count=1; lift.transducers[0].currentCoordinates.pressure=0;
      mt2_session_frame(&s, BT, &lift, 5010, &k);
      CHECK_EQ(r2.n_feed, 2);                                   /* full lift: absence + pump absence */
      CHECK_EQ(r2.feeds[0].contact_count, 0);                       /* 1st absence finalizes the path liftoff */
      CHECK_EQ(r2.feeds[1].contact_count, 0);                       /* 2nd absence pumps the waiting-click flush */
      CHECK_EQ(r2.n_click, 0);                                  /* two empty frames post no spurious click */
      CHECK_EQ(r2.n_arm, 0);                                    /* clean lift: no watchdog */
      rec_t t1={0}; k=mk(&t1); mt2_session_timer(&s,&k);
      CHECK_EQ(t1.n_feed, 0); }                                 /* nothing left to flush */

    /* source switch: a late frame from the OLD source is dropped after reconnect to a new one
       (the real regression the single-active guard defends — the transport-handoff window) */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
      mt2_session_connect(&s, BT,  MT2_EVENT_DRIVEN, &mt2_policy_default, 0);   /* handoff: BT is now active */
      mt2_frame f=one(50);
      mt2_session_frame(&s, USB, &f, 5000, &k); CHECK_EQ(r.n_feed, 0);   /* stale USB frame dropped */
      mt2_session_frame(&s, BT,  &f, 5000, &k); CHECK_EQ(r.n_feed, 1); } /* BT flows */

    /* reconnect re-arms the settle window, but with MT2_SETTLE_MS=0 the window is empty,
       so there is no re-gate: frames flow immediately after a reconnect too. The boot
       flap-storm guard is unneeded — cold-boot measurement found no post-connect burst. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
      mt2_frame f=one(50);
      mt2_session_frame(&s, BT, &f, 5000, &k); CHECK_EQ(r.n_feed, 1);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 10000);             /* reconnect */
      rec_t r2={0}; k=mk(&r2);
      mt2_session_frame(&s, BT, &f, 10000, &k); CHECK_EQ(r2.n_feed, 1);  /* flows at reconnect, no re-gate */
      mt2_session_frame(&s, BT, &f, 10500, &k); CHECK_EQ(r2.n_feed, 2); }/* keeps flowing */

    /* lifecycle: a new contact's FIRST emitted frame is TS_START (MakeTouch),
       subsequent frames are TS_TOUCHING -- the transition tap-to-click needs. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
      mt2_frame f; memset(&f,0,sizeof f);
      f.contact_count=1; f.transducers[0].id=3; f.transducers[0].currentCoordinates.pressure=20; f.transducers[0].state=TS_TOUCHING;
      mt2_session_frame(&s, BT, &f, 5000, &k);
      CHECK_EQ(r.last_feed.transducers[0].state, TS_START);     /* first frame -> MakeTouch */
      mt2_session_frame(&s, BT, &f, 5005, &k);
      CHECK_EQ(r.last_feed.transducers[0].state, TS_TOUCHING); }/* continuation */

    /* lifecycle (STREAMING/USB too): first frame of a contact -> TS_START */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, &mt2_policy_default, 0);
      mt2_frame f; memset(&f,0,sizeof f);
      f.contact_count=1; f.transducers[0].id=7; f.transducers[0].currentCoordinates.pressure=20; f.transducers[0].state=TS_TOUCHING;
      mt2_session_frame(&s, USB, &f, 5000, &k);
      CHECK_EQ(r.last_feed.transducers[0].state, TS_START);
      mt2_session_frame(&s, USB, &f, 5005, &k);
      CHECK_EQ(r.last_feed.transducers[0].state, TS_TOUCHING); }

    /* Silence watchdog: if the stream simply STOPS while a contact is down (no lift
       frame arrives), the armed timer flushes the lift. A full lift emits ONE absence frame
       (the flushed contacts are all BreakTouch -> emit absence only, not BreakTouch+absence). */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
      mt2_frame f; memset(&f,0,sizeof f);
      f.contact_count=1; f.transducers[0].id=2; f.transducers[0].currentCoordinates.pressure=20; f.transducers[0].currentCoordinates.x=88; f.transducers[0].state=TS_TOUCHING;
      mt2_session_frame(&s, BT, &f, 5000, &k);             /* contact down -> START + arm watchdog */
      CHECK_EQ(r.last_feed.transducers[0].state, TS_START);
      CHECK_EQ(r.n_arm, 1); CHECK_EQ(r.last_arm, MT2_IDLE_MS);
      rec_t t={0}; k=mk(&t); mt2_session_timer(&s,&k);     /* stream went silent */
      CHECK_EQ(t.n_feed, 2);                               /* full lift: absence + pump absence */
      CHECK_EQ(t.feeds[0].contact_count, 0);                    /* 1st absence finalizes the lift */
      CHECK_EQ(t.feeds[1].contact_count, 0); }                  /* 2nd absence pumps the waiting-click flush */

    /* EVENT_DRIVEN two-finger physical click: secondary mask survives lift-drop */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, &mt2_policy_default, 0);
      mt2_frame f; memset(&f,0,sizeof f); f.contact_count=2;
      f.transducers[0].currentCoordinates.pressure=20; f.transducers[1].currentCoordinates.pressure=20; f.isPhysicalButtonDown=1;
      mt2_session_frame(&s, BT, &f, 5000, &k);
      CHECK_EQ(r.n_click, 1); CHECK_EQ(r.last_mask, 0x2u); CHECK_EQ(r.n_feed, 1); }

    /* The convergence flips (2026-07) collapsed the two transport rows into one shared
       mt2_policy_default, so the three USB-row difference cases that pinned the old divergence
       (PASSTHROUGH liftoff / empties emitted / no watchdog) were retired. The cases above
       already cover the unified behavior. */
}
TEST_MAIN()
