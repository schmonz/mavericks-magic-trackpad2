#include "mavericks_presence.h"

presence_result_t presence_step(presence_state_t state, presence_event_t event) {
    presence_result_t r = { state, PRESENCE_ACT_NONE };
    switch (event) {
    case PRESENCE_EV_BT_APPEAR:
        if (state != PRESENCE_BT)  { r.next = PRESENCE_BT;  r.action = PRESENCE_ACT_ON_BT; }
        break;
    case PRESENCE_EV_USB_APPEAR:
        if (state != PRESENCE_USB) { r.next = PRESENCE_USB; r.action = PRESENCE_ACT_ON_USB; }
        break;
    case PRESENCE_EV_BT_REMOVE:
        if (state == PRESENCE_BT)  { r.next = PRESENCE_HOLD; r.action = PRESENCE_ACT_HOLD; }
        break;
    case PRESENCE_EV_USB_REMOVE:
        if (state == PRESENCE_USB) { r.next = PRESENCE_HOLD; r.action = PRESENCE_ACT_HOLD; }
        break;
    case PRESENCE_EV_REMOVAL_ELAPSED:
        if (state == PRESENCE_HOLD) { r.next = PRESENCE_NONE; r.action = PRESENCE_ACT_ABSENT; }
        break;
    }
    return r;
}

presence_state_t presence_view_for(int bt_present, int usb_present) {
    if (bt_present)  return PRESENCE_BT;
    if (usb_present) return PRESENCE_USB;
    return PRESENCE_NONE;
}

static presence_action_t render_for(presence_state_t target) {
    switch (target) {
    case PRESENCE_BT:   return PRESENCE_ACT_ON_BT;
    case PRESENCE_USB:  return PRESENCE_ACT_ON_USB;
    case PRESENCE_NONE: return PRESENCE_ACT_ABSENT;
    default:       return PRESENCE_ACT_NONE;
    }
}

presence_result_t presence_reconcile(presence_state_t state, int bt_present, int usb_present) {
    presence_state_t target = presence_view_for(bt_present, usb_present);
    presence_result_t r = { state, PRESENCE_ACT_NONE };
    if (state == PRESENCE_HOLD && target == PRESENCE_NONE) return r;  /* don't fight the hold; timer owns NONE */
    if (state == target) return r;
    r.next = target; r.action = render_for(target);
    return r;
}
