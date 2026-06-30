#include "mt2_pane_sm.h"

psm_result_t psm_step(psm_state_t state, psm_event_t event) {
    psm_result_t r = { state, PSM_ACT_NONE };
    switch (event) {
    case PSM_EV_BT_APPEAR:
        if (state != PSM_BT)  { r.next = PSM_BT;  r.action = PSM_ACT_RENDER_BT; }
        break;
    case PSM_EV_USB_APPEAR:
        if (state != PSM_USB) { r.next = PSM_USB; r.action = PSM_ACT_RENDER_USB; }
        break;
    case PSM_EV_BT_REMOVE:
        if (state == PSM_BT)  { r.next = PSM_HOLD; r.action = PSM_ACT_HOLD; }
        break;
    case PSM_EV_USB_REMOVE:
        if (state == PSM_USB) { r.next = PSM_HOLD; r.action = PSM_ACT_HOLD; }
        break;
    case PSM_EV_REMOVAL_ELAPSED:
        if (state == PSM_HOLD) { r.next = PSM_NONE; r.action = PSM_ACT_RENDER_NONE; }
        break;
    }
    return r;
}

psm_state_t psm_view_for(int bt_present, int usb_present) {
    if (bt_present)  return PSM_BT;
    if (usb_present) return PSM_USB;
    return PSM_NONE;
}

static psm_action_t render_for(psm_state_t target) {
    switch (target) {
    case PSM_BT:   return PSM_ACT_RENDER_BT;
    case PSM_USB:  return PSM_ACT_RENDER_USB;
    case PSM_NONE: return PSM_ACT_RENDER_NONE;
    default:       return PSM_ACT_NONE;
    }
}

psm_result_t psm_reconcile(psm_state_t state, int bt_present, int usb_present) {
    psm_state_t target = psm_view_for(bt_present, usb_present);
    psm_result_t r = { state, PSM_ACT_NONE };
    if (state == PSM_HOLD && target == PSM_NONE) return r;  /* don't fight the hold; timer owns NONE */
    if (state == target) return r;
    r.next = target; r.action = render_for(target);
    return r;
}
