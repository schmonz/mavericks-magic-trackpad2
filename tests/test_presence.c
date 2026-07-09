#include "../src/mt2_presence.h"
#include "test.h"
#include <string.h>

static void check_step(presence_state_t s, presence_event_t e, presence_state_t n, presence_action_t a) {
    presence_result_t r = presence_step(s, e);
    CHECK_EQ(r.next, n);
    CHECK_EQ(r.action, a);
}

/* --- deterministic fake hardware: device (power,cable) -> truth -> events --- */
typedef struct { int power; int cable; } devm_t;   /* power 0/1, cable 0/1 (in=1) */

static void truth_of(devm_t d, int *bt, int *usb) {
    *bt  = (d.power && !d.cable);   /* on + uncabled -> BT */
    *usb = (d.power &&  d.cable);   /* on + cabled   -> USB (always) */
}

typedef enum { M_POWER, M_CABLE } move_t;

static presence_state_t g_state;     /* SM state */
static presence_state_t g_disp;      /* what's on screen (ON_* actions move this; HOLD keeps it) */

static void apply_action(presence_action_t a) {
    switch (a) {
    case PRESENCE_ACT_ON_BT:   g_disp = PRESENCE_BT;   break;
    case PRESENCE_ACT_ON_USB:  g_disp = PRESENCE_USB;  break;
    case PRESENCE_ACT_ABSENT: g_disp = PRESENCE_NONE; break;
    default: break; /* HOLD / NONE: display unchanged */
    }
}

static void feed(presence_event_t e) { presence_result_t r = presence_step(g_state, e); g_state = r.next; apply_action(r.action); }

/* Apply one move: emit removes-before-appears for the truth delta, then settle the hold timer. */
static devm_t do_move(devm_t d, move_t m, int *saw_usb, int *saw_none) {
    int bt0,usb0,bt1,usb1;
    truth_of(d, &bt0, &usb0);
    devm_t n = d;
    if (m == M_POWER) n.power = !n.power; else n.cable = !n.cable;
    truth_of(n, &bt1, &usb1);
    if (bt0  && !bt1)  feed(PRESENCE_EV_BT_REMOVE);
    if (usb0 && !usb1) feed(PRESENCE_EV_USB_REMOVE);
    if (!bt0  && bt1)  feed(PRESENCE_EV_BT_APPEAR);
    if (!usb0 && usb1) feed(PRESENCE_EV_USB_APPEAR);
    feed(PRESENCE_EV_REMOVAL_ELAPSED);                 /* settle any lingering hold */
    if (saw_usb  && g_disp == PRESENCE_USB)  *saw_usb  = 1;
    if (saw_none && g_disp == PRESENCE_NONE) *saw_none = 1;
    return n;
}

/* Reset SM+display to match a device's truth (start each walk from a known-correct state). */
static void reset_to(devm_t d) {
    int bt,usb; truth_of(d, &bt, &usb);
    g_state = presence_view_for(bt, usb);
    g_disp  = g_state;
}

static void run_tests(void) {
    /* APPEAR(X): converge to X (render) unless already showing X (idempotent no-op). */
    check_step(PRESENCE_NONE, PRESENCE_EV_BT_APPEAR,  PRESENCE_BT,  PRESENCE_ACT_ON_BT);
    check_step(PRESENCE_NONE, PRESENCE_EV_USB_APPEAR, PRESENCE_USB, PRESENCE_ACT_ON_USB);
    check_step(PRESENCE_BT,   PRESENCE_EV_BT_APPEAR,  PRESENCE_BT,  PRESENCE_ACT_NONE);
    check_step(PRESENCE_USB,  PRESENCE_EV_USB_APPEAR, PRESENCE_USB, PRESENCE_ACT_NONE);
    check_step(PRESENCE_USB,  PRESENCE_EV_BT_APPEAR,  PRESENCE_BT,  PRESENCE_ACT_ON_BT);   /* single-transport: BT now wins */
    check_step(PRESENCE_BT,   PRESENCE_EV_USB_APPEAR, PRESENCE_USB, PRESENCE_ACT_ON_USB);

    /* REMOVE(X): enter HOLD only if currently showing X; else a stale/dup no-op. */
    check_step(PRESENCE_BT,   PRESENCE_EV_BT_REMOVE,  PRESENCE_HOLD, PRESENCE_ACT_HOLD);
    check_step(PRESENCE_USB,  PRESENCE_EV_USB_REMOVE, PRESENCE_HOLD, PRESENCE_ACT_HOLD);
    check_step(PRESENCE_USB,  PRESENCE_EV_BT_REMOVE,  PRESENCE_USB,  PRESENCE_ACT_NONE);
    check_step(PRESENCE_NONE, PRESENCE_EV_BT_REMOVE,  PRESENCE_NONE, PRESENCE_ACT_NONE);

    /* HOLD resolves: an appear snaps to that transport; the timer falls to NoTrackpad. */
    check_step(PRESENCE_HOLD, PRESENCE_EV_USB_APPEAR,      PRESENCE_USB,  PRESENCE_ACT_ON_USB);
    check_step(PRESENCE_HOLD, PRESENCE_EV_BT_APPEAR,       PRESENCE_BT,   PRESENCE_ACT_ON_BT);
    check_step(PRESENCE_HOLD, PRESENCE_EV_REMOVAL_ELAPSED, PRESENCE_NONE, PRESENCE_ACT_ABSENT);

    /* REMOVAL_ELAPSED outside HOLD is a no-op. */
    check_step(PRESENCE_BT,   PRESENCE_EV_REMOVAL_ELAPSED, PRESENCE_BT,   PRESENCE_ACT_NONE);
    check_step(PRESENCE_NONE, PRESENCE_EV_REMOVAL_ELAPSED, PRESENCE_NONE, PRESENCE_ACT_NONE);

    /* presence_view_for: deterministic truth -> settled view (single transport; BT wins if both). */
    CHECK_EQ(presence_view_for(0,0), PRESENCE_NONE);
    CHECK_EQ(presence_view_for(1,0), PRESENCE_BT);
    CHECK_EQ(presence_view_for(0,1), PRESENCE_USB);
    CHECK_EQ(presence_view_for(1,1), PRESENCE_BT);

    /* presence_reconcile: converge to truth; no-op when already correct. */
    { presence_result_t r = presence_reconcile(PRESENCE_NONE, 0, 1); CHECK_EQ(r.next, PRESENCE_USB); CHECK_EQ(r.action, PRESENCE_ACT_ON_USB); }
    { presence_result_t r = presence_reconcile(PRESENCE_USB, 0, 1);  CHECK_EQ(r.next, PRESENCE_USB); CHECK_EQ(r.action, PRESENCE_ACT_NONE); }
    { presence_result_t r = presence_reconcile(PRESENCE_BT,  0, 1);  CHECK_EQ(r.next, PRESENCE_USB); CHECK_EQ(r.action, PRESENCE_ACT_ON_USB); }
    { presence_result_t r = presence_reconcile(PRESENCE_NONE,1, 0);  CHECK_EQ(r.next, PRESENCE_BT);  CHECK_EQ(r.action, PRESENCE_ACT_ON_BT); }
    { presence_result_t r = presence_reconcile(PRESENCE_BT,  0, 0);  CHECK_EQ(r.next, PRESENCE_NONE);CHECK_EQ(r.action, PRESENCE_ACT_ABSENT); }
    /* self-heal a missed USB appear that left us stuck NONE (the stuck-NoTrackpad bug): */
    { presence_result_t r = presence_reconcile(PRESENCE_NONE,0, 1);  CHECK_EQ(r.next, PRESENCE_USB); CHECK_EQ(r.action, PRESENCE_ACT_ON_USB); }
    /* during a hold, transient truth==NONE must NOT force NONE (handoff window owns that): */
    { presence_result_t r = presence_reconcile(PRESENCE_HOLD,0, 0);  CHECK_EQ(r.next, PRESENCE_HOLD);CHECK_EQ(r.action, PRESENCE_ACT_NONE); }
    /* but a hold may self-heal to a present transport: */
    { presence_result_t r = presence_reconcile(PRESENCE_HOLD,0, 1);  CHECK_EQ(r.next, PRESENCE_USB); CHECK_EQ(r.action, PRESENCE_ACT_ON_USB); }

    /* INVARIANT: from every device state, after any move sequence settles,
     * displayed == view_for(truth). Exhausts singles, pairs, triples, and 4-move paths. */
    devm_t all[4]; int k = 0;
    for (int p = 0; p < 2; p++) for (int c = 0; c < 2; c++) { all[k].power = p; all[k].cable = c; k++; }
    move_t moves[2] = { M_POWER, M_CABLE };

    for (int depth = 1; depth <= 4; depth++) {
        int combos = 1; for (int i = 0; i < depth; i++) combos *= 2;   /* 2^depth move-sequences */
        for (int start = 0; start < 4; start++) {
            for (int seq = 0; seq < combos; seq++) {
                reset_to(all[start]);
                devm_t d = all[start];
                for (int i = 0; i < depth; i++) {
                    move_t mv = moves[(seq >> i) & 1];
                    d = do_move(d, mv, 0, 0);
                    int bt,usb; truth_of(d, &bt, &usb);
                    CHECK_EQ(g_disp, presence_view_for(bt, usb));   /* invariant after every move */
                }
            }
        }
    }

    /* --- regression sequences: each on-device bug, as a deterministic test --- */

    /* (a) power-off ON BT: BT view -> NoTrackpad, NEVER a USB-look in between. */
    { devm_t d = { 1, 0 }; reset_to(d); int saw_usb = 0;
      d = do_move(d, M_POWER, &saw_usb, 0);                  /* power off */
      CHECK_EQ(g_disp, PRESENCE_NONE); CHECK_EQ(saw_usb, 0); }

    /* (b) handoff BT->USB (plug cable while on): snap to USB, NEVER NoTrackpad in between. */
    { devm_t d = { 1, 0 }; reset_to(d); int saw_none = 0;
      d = do_move(d, M_CABLE, 0, &saw_none);                 /* cable in */
      CHECK_EQ(g_disp, PRESENCE_USB); CHECK_EQ(saw_none, 0); }

    /* (c) power-on while cabled -> USB with battery hidden (the stale-BT bug). */
    { devm_t d = { 0, 1 }; reset_to(d); int saw_none = 0;
      d = do_move(d, M_POWER, 0, &saw_none);                 /* power on, cable already in */
      CHECK_EQ(g_disp, PRESENCE_USB); }

    /* (d) dropped USB appear (manually-started driver, no FirstMatch) -> reconcile self-heals.
     *     Simulate: device is on USB (truth), but the SM never saw the appear (stuck NONE). */
    { g_state = PRESENCE_NONE; g_disp = PRESENCE_NONE;                 /* stuck on NoTrackpad */
      presence_result_t r = presence_reconcile(g_state, 0, 1);         /* poll reads truth = USB */
      g_state = r.next; apply_action(r.action);
      CHECK_EQ(g_disp, PRESENCE_USB); }
}
TEST_MAIN()
