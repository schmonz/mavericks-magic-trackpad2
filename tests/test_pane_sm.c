#include "../src/mt2_pane_sm.h"
#include "test.h"
#include <string.h>

static void check_step(psm_state_t s, psm_event_t e, psm_state_t n, psm_action_t a) {
    psm_result_t r = psm_step(s, e);
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

static psm_state_t g_state;     /* SM state */
static psm_state_t g_disp;      /* what's on screen (RENDER_* actions move this; HOLD keeps it) */

static void apply_action(psm_action_t a) {
    switch (a) {
    case PSM_ACT_RENDER_BT:   g_disp = PSM_BT;   break;
    case PSM_ACT_RENDER_USB:  g_disp = PSM_USB;  break;
    case PSM_ACT_RENDER_NONE: g_disp = PSM_NONE; break;
    default: break; /* HOLD / NONE: display unchanged */
    }
}

static void feed(psm_event_t e) { psm_result_t r = psm_step(g_state, e); g_state = r.next; apply_action(r.action); }

/* Apply one move: emit removes-before-appears for the truth delta, then settle the hold timer. */
static devm_t do_move(devm_t d, move_t m, int *saw_usb, int *saw_none) {
    int bt0,usb0,bt1,usb1;
    truth_of(d, &bt0, &usb0);
    devm_t n = d;
    if (m == M_POWER) n.power = !n.power; else n.cable = !n.cable;
    truth_of(n, &bt1, &usb1);
    if (bt0  && !bt1)  feed(PSM_EV_BT_REMOVE);
    if (usb0 && !usb1) feed(PSM_EV_USB_REMOVE);
    if (!bt0  && bt1)  feed(PSM_EV_BT_APPEAR);
    if (!usb0 && usb1) feed(PSM_EV_USB_APPEAR);
    feed(PSM_EV_REMOVAL_ELAPSED);                 /* settle any lingering hold */
    if (saw_usb  && g_disp == PSM_USB)  *saw_usb  = 1;
    if (saw_none && g_disp == PSM_NONE) *saw_none = 1;
    return n;
}

/* Reset SM+display to match a device's truth (start each walk from a known-correct state). */
static void reset_to(devm_t d) {
    int bt,usb; truth_of(d, &bt, &usb);
    g_state = psm_view_for(bt, usb);
    g_disp  = g_state;
}

static void run_tests(void) {
    /* APPEAR(X): converge to X (render) unless already showing X (idempotent no-op). */
    check_step(PSM_NONE, PSM_EV_BT_APPEAR,  PSM_BT,  PSM_ACT_RENDER_BT);
    check_step(PSM_NONE, PSM_EV_USB_APPEAR, PSM_USB, PSM_ACT_RENDER_USB);
    check_step(PSM_BT,   PSM_EV_BT_APPEAR,  PSM_BT,  PSM_ACT_NONE);
    check_step(PSM_USB,  PSM_EV_USB_APPEAR, PSM_USB, PSM_ACT_NONE);
    check_step(PSM_USB,  PSM_EV_BT_APPEAR,  PSM_BT,  PSM_ACT_RENDER_BT);   /* single-transport: BT now wins */
    check_step(PSM_BT,   PSM_EV_USB_APPEAR, PSM_USB, PSM_ACT_RENDER_USB);

    /* REMOVE(X): enter HOLD only if currently showing X; else a stale/dup no-op. */
    check_step(PSM_BT,   PSM_EV_BT_REMOVE,  PSM_HOLD, PSM_ACT_HOLD);
    check_step(PSM_USB,  PSM_EV_USB_REMOVE, PSM_HOLD, PSM_ACT_HOLD);
    check_step(PSM_USB,  PSM_EV_BT_REMOVE,  PSM_USB,  PSM_ACT_NONE);
    check_step(PSM_NONE, PSM_EV_BT_REMOVE,  PSM_NONE, PSM_ACT_NONE);

    /* HOLD resolves: an appear snaps to that transport; the timer falls to NoTrackpad. */
    check_step(PSM_HOLD, PSM_EV_USB_APPEAR,      PSM_USB,  PSM_ACT_RENDER_USB);
    check_step(PSM_HOLD, PSM_EV_BT_APPEAR,       PSM_BT,   PSM_ACT_RENDER_BT);
    check_step(PSM_HOLD, PSM_EV_REMOVAL_ELAPSED, PSM_NONE, PSM_ACT_RENDER_NONE);

    /* REMOVAL_ELAPSED outside HOLD is a no-op. */
    check_step(PSM_BT,   PSM_EV_REMOVAL_ELAPSED, PSM_BT,   PSM_ACT_NONE);
    check_step(PSM_NONE, PSM_EV_REMOVAL_ELAPSED, PSM_NONE, PSM_ACT_NONE);

    /* psm_view_for: deterministic truth -> settled view (single transport; BT wins if both). */
    CHECK_EQ(psm_view_for(0,0), PSM_NONE);
    CHECK_EQ(psm_view_for(1,0), PSM_BT);
    CHECK_EQ(psm_view_for(0,1), PSM_USB);
    CHECK_EQ(psm_view_for(1,1), PSM_BT);

    /* psm_reconcile: converge to truth; no-op when already correct. */
    { psm_result_t r = psm_reconcile(PSM_NONE, 0, 1); CHECK_EQ(r.next, PSM_USB); CHECK_EQ(r.action, PSM_ACT_RENDER_USB); }
    { psm_result_t r = psm_reconcile(PSM_USB, 0, 1);  CHECK_EQ(r.next, PSM_USB); CHECK_EQ(r.action, PSM_ACT_NONE); }
    { psm_result_t r = psm_reconcile(PSM_BT,  0, 1);  CHECK_EQ(r.next, PSM_USB); CHECK_EQ(r.action, PSM_ACT_RENDER_USB); }
    { psm_result_t r = psm_reconcile(PSM_NONE,1, 0);  CHECK_EQ(r.next, PSM_BT);  CHECK_EQ(r.action, PSM_ACT_RENDER_BT); }
    { psm_result_t r = psm_reconcile(PSM_BT,  0, 0);  CHECK_EQ(r.next, PSM_NONE);CHECK_EQ(r.action, PSM_ACT_RENDER_NONE); }
    /* self-heal a missed USB appear that left us stuck NONE (the stuck-NoTrackpad bug): */
    { psm_result_t r = psm_reconcile(PSM_NONE,0, 1);  CHECK_EQ(r.next, PSM_USB); CHECK_EQ(r.action, PSM_ACT_RENDER_USB); }
    /* during a hold, transient truth==NONE must NOT force NONE (handoff window owns that): */
    { psm_result_t r = psm_reconcile(PSM_HOLD,0, 0);  CHECK_EQ(r.next, PSM_HOLD);CHECK_EQ(r.action, PSM_ACT_NONE); }
    /* but a hold may self-heal to a present transport: */
    { psm_result_t r = psm_reconcile(PSM_HOLD,0, 1);  CHECK_EQ(r.next, PSM_USB); CHECK_EQ(r.action, PSM_ACT_RENDER_USB); }

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
                    CHECK_EQ(g_disp, psm_view_for(bt, usb));   /* invariant after every move */
                }
            }
        }
    }
}
TEST_MAIN()
