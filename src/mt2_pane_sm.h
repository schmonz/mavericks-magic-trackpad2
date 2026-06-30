#ifndef MT2_PANE_SM_H
#define MT2_PANE_SM_H

/* Pure transport state machine for the Trackpad prefpane. No IOKit, no Cocoa, no globals.
 * Sibling of mt2_connect_sm. The osax adapter translates IOKit edges/timers/poll into events,
 * calls these, and performs the returned action. */

typedef enum {
    PSM_NONE,   /* showing NoTrackpad */
    PSM_BT,     /* showing Trackpad WITH battery (BT) */
    PSM_USB,    /* showing Trackpad WITHOUT battery (USB) */
    PSM_HOLD    /* transient: a removal happened; keep the current view until the window resolves */
} psm_state_t;

typedef enum {
    PSM_EV_BT_APPEAR, PSM_EV_BT_REMOVE,
    PSM_EV_USB_APPEAR, PSM_EV_USB_REMOVE,
    PSM_EV_REMOVAL_ELAPSED      /* the removal-window timer fired */
} psm_event_t;

typedef enum {
    PSM_ACT_NONE,
    PSM_ACT_RENDER_NONE,   /* loadMainView -> NoTrackpad */
    PSM_ACT_RENDER_BT,     /* ensure Trackpad view + battery SHOWN  (real _magicTrackpadAction connected=1) */
    PSM_ACT_RENDER_USB,    /* ensure Trackpad view + battery HIDDEN (real _magicTrackpadAction connected=0) */
    PSM_ACT_HOLD           /* keep current view; adapter arms the removal-window timer */
} psm_action_t;

typedef struct { psm_state_t next; psm_action_t action; } psm_result_t;

/* Pure edge step. Unknown/duplicate events are no-ops: (stay in state, PSM_ACT_NONE). */
psm_result_t psm_step(psm_state_t state, psm_event_t event);

/* The settled view a given device truth must produce (single transport at a time; if both are
 * set, BT wins — shouldn't happen). This is the test invariant + the reconcile target. */
psm_state_t psm_view_for(int bt_present, int usb_present);

/* Reconcile against truth (the self-heal for missed/manually-started edges). Forces convergence
 * to psm_view_for(truth). EXCEPTION: never forces NONE while holding — a handoff's window has a
 * transient truth==NONE (BT gone, USB not yet enumerated); the REMOVAL_ELAPSED timer owns the
 * NONE transition, so reconcile stays in PSM_HOLD when target==PSM_NONE. */
psm_result_t psm_reconcile(psm_state_t state, int bt_present, int usb_present);

#endif
