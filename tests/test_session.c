#include "../src/mt2_session.h"
#include "test.h"
#include <string.h>

typedef struct {
    int n_click; unsigned last_mask;
    int n_feed;  touch_frame_t last_feed; touch_frame_t feeds[8];
    int n_arm;   uint32_t last_arm;
} rec_t;
static void rec_click(void *c, unsigned m){ rec_t*r=c; r->n_click++; r->last_mask=m; }
static void rec_feed (void *c, const touch_frame_t *f){ rec_t*r=c; if(r->n_feed<8) r->feeds[r->n_feed]=*f; r->n_feed++; r->last_feed=*f; }
static void rec_arm  (void *c, uint32_t ms){ rec_t*r=c; r->n_arm++; r->last_arm=ms; }
static mt2_session_sink_t mk(rec_t *r){
    mt2_session_sink_t s; s.post_click=rec_click; s.feed_frame=rec_feed; s.arm_timer=rec_arm; s.ctx=r; return s;
}
#define BT  0xB7
#define USB 0x5B

static touch_frame_t one(int x){ touch_frame_t f; memset(&f,0,sizeof f);
    f.ntouches=1; f.touches[0].size=20; f.touches[0].x=x; return f; }

static void run_tests(void) {
    /* settle gate calibrated to 0 (MT2_SETTLE_MS): cold-boot measurement on both
       transports showed NO post-connect burst reaching the pipeline (interrupt/event
       endpoints deliver only on touch), so frames flow immediately from connect with
       no startup delay. The gate mechanism is retained (mt2_settle_passed, covered in
       test_pipeline) as a zero-cost seam should a future device ever need it. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 1000);
      touch_frame_t f=one(50);
      mt2_session_frame(&s, BT, &f, 1000, &k); CHECK_EQ(r.n_feed, 1);   /* flows at connect, no gate */
      mt2_session_frame(&s, BT, &f, 1500, &k); CHECK_EQ(r.n_feed, 2); } /* keeps flowing */

    /* single-active guard: non-active source ignored */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t f=one(50);
      mt2_session_frame(&s, BT,  &f, 9999, &k); CHECK_EQ(r.n_feed, 0);
      mt2_session_frame(&s, USB, &f, 9999, &k); CHECK_EQ(r.n_feed, 1); }

    /* A lone lift frame with no contact ever down produces nothing (no phantom feed). */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t lift; memset(&lift,0,sizeof lift); lift.ntouches=1; lift.touches[0].size=0;
      mt2_session_frame(&s, USB, &lift, 5000, &k);
      CHECK_EQ(r.n_feed, 0); CHECK_EQ(r.n_arm, 0); }

    /* click: two-finger press -> secondary then feed (post-settle) */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t f; memset(&f,0,sizeof f); f.ntouches=2;
      f.touches[0].size=20; f.touches[1].size=20; f.button=1;
      mt2_session_frame(&s, USB, &f, 5000, &k);
      CHECK_EQ(r.n_click, 1); CHECK_EQ(r.last_mask, 0x2u); CHECK_EQ(r.n_feed, 1); }

    /* EVENT_DRIVEN real contact: lift-drop applied, feed, arm IDLE */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 0);
      touch_frame_t f; memset(&f,0,sizeof f); f.ntouches=2;
      f.touches[0].size=20; f.touches[0].x=70; f.touches[1].size=0;   /* one lifted */
      mt2_session_frame(&s, BT, &f, 5000, &k);
      CHECK_EQ(r.n_feed, 1); CHECK_EQ(r.last_feed.ntouches, 1);
      CHECK_EQ(r.n_arm, 1); CHECK_EQ(r.last_arm, MT2_IDLE_MS); }

    /* EVENT_DRIVEN clean lift: the lift frame emits a BreakTouch (TS_END) for the ended
       contact at its last-known position and arms NO watchdog (cleanly lifted). The native
       BreakTouch replaces the old held-replay deceleration. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 0);
      touch_frame_t real=one(70); mt2_session_frame(&s, BT, &real, 5000, &k);
      rec_t r2={0}; k=mk(&r2);
      touch_frame_t lift; memset(&lift,0,sizeof lift); lift.ntouches=1; lift.touches[0].size=0;
      mt2_session_frame(&s, BT, &lift, 5010, &k);
      CHECK_EQ(r2.n_feed, 2);                                   /* BreakTouch, then trailing absence */
      CHECK_EQ(r2.feeds[0].ntouches, 1);
      CHECK_EQ(r2.feeds[0].touches[0].state, TS_END);
      CHECK_EQ(r2.feeds[0].touches[0].x, 70);                  /* last-known position */
      CHECK_EQ(r2.feeds[0].ts_offset_ms, 0u);                  /* lift frame stamped at "now" */
      CHECK_EQ(r2.feeds[1].ntouches, 0);                       /* absence frame finalizes the path liftoff */
      CHECK_EQ(r2.feeds[1].ts_offset_ms, MT2_LIFTOFF_GAP_MS);  /* ...a gap LATER, so not coincident with the lift */
      CHECK_EQ(r2.n_arm, 0);                                    /* clean lift: no watchdog */
      rec_t t1={0}; k=mk(&t1); mt2_session_timer(&s,&k);
      CHECK_EQ(t1.n_feed, 0); }                                 /* nothing left to flush */

    /* source switch: a late frame from the OLD source is dropped after reconnect to a new one
       (the real regression the single-active guard defends — the transport-handoff window) */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      mt2_session_connect(&s, BT,  MT2_EVENT_DRIVEN, 0);   /* handoff: BT is now active */
      touch_frame_t f=one(50);
      mt2_session_frame(&s, USB, &f, 5000, &k); CHECK_EQ(r.n_feed, 0);   /* stale USB frame dropped */
      mt2_session_frame(&s, BT,  &f, 5000, &k); CHECK_EQ(r.n_feed, 1); } /* BT flows */

    /* reconnect re-arms the settle window, but with MT2_SETTLE_MS=0 the window is empty,
       so there is no re-gate: frames flow immediately after a reconnect too. The boot
       flap-storm guard is unneeded — cold-boot measurement found no post-connect burst. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 0);
      touch_frame_t f=one(50);
      mt2_session_frame(&s, BT, &f, 5000, &k); CHECK_EQ(r.n_feed, 1);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 10000);             /* reconnect */
      rec_t r2={0}; k=mk(&r2);
      mt2_session_frame(&s, BT, &f, 10000, &k); CHECK_EQ(r2.n_feed, 1);  /* flows at reconnect, no re-gate */
      mt2_session_frame(&s, BT, &f, 10500, &k); CHECK_EQ(r2.n_feed, 2); }/* keeps flowing */

    /* lifecycle: a new contact's FIRST emitted frame is TS_START (MakeTouch),
       subsequent frames are TS_TOUCHING -- the transition tap-to-click needs. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 0);
      touch_frame_t f; memset(&f,0,sizeof f);
      f.ntouches=1; f.touches[0].id=3; f.touches[0].size=20; f.touches[0].state=TS_TOUCHING;
      mt2_session_frame(&s, BT, &f, 5000, &k);
      CHECK_EQ(r.last_feed.touches[0].state, TS_START);     /* first frame -> MakeTouch */
      mt2_session_frame(&s, BT, &f, 5005, &k);
      CHECK_EQ(r.last_feed.touches[0].state, TS_TOUCHING); }/* continuation */

    /* lifecycle (STREAMING/USB too): first frame of a contact -> TS_START */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t f; memset(&f,0,sizeof f);
      f.ntouches=1; f.touches[0].id=7; f.touches[0].size=20; f.touches[0].state=TS_TOUCHING;
      mt2_session_frame(&s, USB, &f, 5000, &k);
      CHECK_EQ(r.last_feed.touches[0].state, TS_START);
      mt2_session_frame(&s, USB, &f, 5005, &k);
      CHECK_EQ(r.last_feed.touches[0].state, TS_TOUCHING); }

    /* Silence watchdog: if the stream simply STOPS while a contact is down (no lift
       frame arrives), the armed timer flushes a BreakTouch so the lift still registers. */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 0);
      touch_frame_t f; memset(&f,0,sizeof f);
      f.ntouches=1; f.touches[0].id=2; f.touches[0].size=20; f.touches[0].x=88; f.touches[0].state=TS_TOUCHING;
      mt2_session_frame(&s, BT, &f, 5000, &k);             /* contact down -> START + arm watchdog */
      CHECK_EQ(r.last_feed.touches[0].state, TS_START);
      CHECK_EQ(r.n_arm, 1); CHECK_EQ(r.last_arm, MT2_IDLE_MS);
      rec_t t={0}; k=mk(&t); mt2_session_timer(&s,&k);     /* stream went silent */
      CHECK_EQ(t.n_feed, 2);                               /* BreakTouch, then trailing absence */
      CHECK_EQ(t.feeds[0].touches[0].state, TS_END);
      CHECK_EQ(t.feeds[0].touches[0].x, 88);
      CHECK_EQ(t.feeds[1].ntouches, 0);                    /* absence finalizes the lift */
      CHECK_EQ(t.feeds[1].ts_offset_ms, MT2_LIFTOFF_GAP_MS); }  /* spaced after the lift */

    /* EVENT_DRIVEN two-finger physical click: secondary mask survives lift-drop */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 0);
      touch_frame_t f; memset(&f,0,sizeof f); f.ntouches=2;
      f.touches[0].size=20; f.touches[1].size=20; f.button=1;
      mt2_session_frame(&s, BT, &f, 5000, &k);
      CHECK_EQ(r.n_click, 1); CHECK_EQ(r.last_mask, 0x2u); CHECK_EQ(r.n_feed, 1); }
}
TEST_MAIN()
