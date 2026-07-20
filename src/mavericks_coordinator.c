#include "mavericks_coordinator.h"

bool mavericks_coordinator_activate(mt2_transport_id_t transport, unsigned long device_id) {
    (void)transport; (void)device_id;
    return true;   /* MT2: single-transport device — never suppress. Future devices override here. */
}
