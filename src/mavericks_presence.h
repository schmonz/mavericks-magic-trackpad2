#ifndef MAVERICKS_PRESENCE_H
#define MAVERICKS_PRESENCE_H

/* Pure transport-presence state machine. No IOKit, no Cocoa, no globals.
 * Sibling of mavericks_connect_sm (bring-up) and the arbitration concern in mavericks_session/mavericks_coordinator.
 * An adapter translates IOKit device edges/timers/poll into presence_event_t, calls these, and
 * interprets the returned action for its consumer (the prefpane renders it; the handoff acts on it). */

typedef enum {
    PRESENCE_NONE,   /* showing NoTrackpad */
    PRESENCE_BT,     /* showing Trackpad WITH battery (BT) */
    PRESENCE_USB,    /* showing Trackpad WITHOUT battery (USB) */
    PRESENCE_HOLD    /* transient: a removal happened; keep the current view until the window resolves */
} presence_state_t;

typedef enum {
    PRESENCE_EV_BT_APPEAR, PRESENCE_EV_BT_REMOVE,
    PRESENCE_EV_USB_APPEAR, PRESENCE_EV_USB_REMOVE,
    PRESENCE_EV_REMOVAL_ELAPSED      /* the removal-window timer fired */
} presence_event_t;

typedef enum {
    PRESENCE_ACT_NONE,
    PRESENCE_ACT_ABSENT,   /* presence went to none (no transport) */
    PRESENCE_ACT_ON_BT,    /* presence is now Bluetooth */
    PRESENCE_ACT_ON_USB,   /* presence is now USB */
    PRESENCE_ACT_HOLD      /* transient: a removal happened; hold current truth until the window resolves */
} presence_action_t;

typedef struct { presence_state_t next; presence_action_t action; } presence_result_t;

/* Pure edge step. Unknown/duplicate events are no-ops: (stay in state, PRESENCE_ACT_NONE). */
presence_result_t presence_step(presence_state_t state, presence_event_t event);

/* The settled view a given device truth must produce (single transport at a time; if both are
 * set, BT wins — shouldn't happen). This is the test invariant + the reconcile target. */
presence_state_t presence_view_for(int bt_present, int usb_present);

/* Reconcile against truth (the self-heal for missed/manually-started edges). Forces convergence
 * to presence_view_for(truth). EXCEPTION: never forces NONE while holding — a handoff's window has a
 * transient truth==NONE (BT gone, USB not yet enumerated); the REMOVAL_ELAPSED timer owns the
 * NONE transition, so reconcile stays in PRESENCE_HOLD when target==PRESENCE_NONE. */
presence_result_t presence_reconcile(presence_state_t state, int bt_present, int usb_present);

#endif
