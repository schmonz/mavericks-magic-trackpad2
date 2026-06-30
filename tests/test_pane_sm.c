#include "../src/mt2_pane_sm.h"
#include "test.h"

static void check_step(psm_state_t s, psm_event_t e, psm_state_t n, psm_action_t a) {
    psm_result_t r = psm_step(s, e);
    CHECK_EQ(r.next, n);
    CHECK_EQ(r.action, a);
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
}
TEST_MAIN()
