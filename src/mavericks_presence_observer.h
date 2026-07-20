#ifndef MAVERICKS_PRESENCE_OBSERVER_H
#define MAVERICKS_PRESENCE_OBSERVER_H
#include "mavericks_presence.h"

/* Shared IOKit adapter over the pure mavericks_presence SM: arms FirstMatch+Terminated notifications for the
 * MT2's USB (AppleUSBMultitouchDriver) and BT (BNBTrackpadDevice) services, drives one presence SM
 * (including the removal-window HOLD timer + supersession), and reports each transition to a consumer
 * callback. The prefpane osax and the USB->BT handoff daemon are the two consumers — same presence
 * truth, one place. This is the IOKit-bound half; the decision logic is the host-tested mavericks_presence. */

/* Called on each presence transition with the action the SM produced AND the event that drove it
 * (runs on the main queue). Consumers that render use `action` (the prefpane); consumers that need a
 * raw transport edge use `event` (the USB->BT handoff keys on PRESENCE_EV_USB_REMOVE — a HOLD action
 * alone can't tell a USB drop from a BT drop). Poll-driven reconcile corrections carry
 * PRESENCE_EV_REMOVAL_ELAPSED as a "no discrete edge" placeholder. */
typedef void (*presence_on_transition_t)(presence_action_t action, presence_event_t event, void *ctx);

typedef struct mavericks_presence_observer mavericks_presence_observer_t;

/* Arm the notifications on the given CFRunLoop, drive one internal presence SM (removal_ms = the
 * coalescing window; the pane uses 1300), and invoke cb on each transition. NULL on failure. */
mavericks_presence_observer_t *presence_observer_create(void *runloop /* CFRunLoopRef */,
                                                  int removal_ms,
                                                  presence_on_transition_t cb, void *ctx);
void presence_observer_destroy(mavericks_presence_observer_t *o);

/* Current settled presence (for consumers that also read state, e.g. the pane's battery paint).
 * NULL observer -> PRESENCE_NONE (matches the pre-create initial state). */
presence_state_t presence_observer_state(const mavericks_presence_observer_t *o);

/* Self-heal poll: re-derive truth from live service presence and apply presence_reconcile (fires cb on
 * a correction). Consumers with a periodic tick call this; edge-only consumers don't need it. */
void presence_observer_reconcile(mavericks_presence_observer_t *o);

#endif
