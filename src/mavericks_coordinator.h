#ifndef MAVERICKS_COORDINATOR_H
#define MAVERICKS_COORDINATOR_H
#include <stdbool.h>

/*
 * mavericks_coordinator — transport-coordinator seam. A reader calls mavericks_coordinator_activate() once it has
 * established, to ask whether it should proceed to drive the device. The MT2 only ever drives ONE
 * transport at a time (cabling USB drops BT, and vice versa), so for the MT2 this is unconditionally
 * true — there is nothing to arbitrate. The seam exists for FUTURE devices that CAN double-drive and
 * must suppress the other transport; such a device's policy would replace the body here (or become a
 * row in the phase-2 device table).
 */

typedef enum { MT2_XPORT_BT = 0, MT2_XPORT_USB = 1 } mt2_transport_id_t;

/* True ⇒ proceed to drive the device on this transport. */
bool mavericks_coordinator_activate(mt2_transport_id_t transport, unsigned long device_id);

#endif
