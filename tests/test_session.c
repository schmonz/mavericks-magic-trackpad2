#include "../src/mt2_session.h"
#include "test.h"
#include <string.h>

typedef struct {
    int n_click; unsigned last_mask;
    int n_feed;  touch_frame_t last_feed;
    int n_arm;   uint32_t last_arm;
} rec_t;
static void rec_click(void *c, unsigned m){ rec_t*r=c; r->n_click++; r->last_mask=m; }
static void rec_feed (void *c, const touch_frame_t *f){ rec_t*r=c; r->n_feed++; r->last_feed=*f; }
static void rec_arm  (void *c, uint32_t ms){ rec_t*r=c; r->n_arm++; r->last_arm=ms; }
static mt2_session_sink_t mk(rec_t *r){
    mt2_session_sink_t s; s.post_click=rec_click; s.feed_frame=rec_feed; s.arm_timer=rec_arm; s.ctx=r; return s;
}
#define BT  0xB7
#define USB 0x5B

static touch_frame_t one(int x){ touch_frame_t f; memset(&f,0,sizeof f);
    f.ntouches=1; f.touches[0].size=20; f.touches[0].x=x; return f; }

static void run_tests(void) {
    /* settle gate: drops inside window, flows after */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, BT, MT2_EVENT_DRIVEN, 1000);
      touch_frame_t f=one(50);
      mt2_session_frame(&s, BT, &f, 1500, &k); CHECK_EQ(r.n_feed, 0);   /* 500<2500 */
      mt2_session_frame(&s, BT, &f, 4000, &k); CHECK_EQ(r.n_feed, 1); } /* settled */

    /* single-active guard: non-active source ignored */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t f=one(50);
      mt2_session_frame(&s, BT,  &f, 9999, &k); CHECK_EQ(r.n_feed, 0);
      mt2_session_frame(&s, USB, &f, 9999, &k); CHECK_EQ(r.n_feed, 1); }

    /* STREAMING passes a lift through and arms NO timer */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t lift; memset(&lift,0,sizeof lift); lift.ntouches=1; lift.touches[0].size=0;
      mt2_session_frame(&s, USB, &lift, 5000, &k);
      CHECK_EQ(r.n_feed, 1); CHECK_EQ(r.n_arm, 0); }

    /* click: two-finger press -> secondary then feed (post-settle) */
    { mt2_session_t s; memset(&s,0,sizeof s); rec_t r={0}; mt2_session_sink_t k=mk(&r);
      mt2_session_connect(&s, USB, MT2_STREAMING, 0);
      touch_frame_t f; memset(&f,0,sizeof f); f.ntouches=2;
      f.touches[0].size=20; f.touches[1].size=20; f.button=1;
      mt2_session_frame(&s, USB, &f, 5000, &k);
      CHECK_EQ(r.n_click, 1); CHECK_EQ(r.last_mask, 0x2u); CHECK_EQ(r.n_feed, 1); }
}
TEST_MAIN()
