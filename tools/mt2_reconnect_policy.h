#ifndef MT2_RECONNECT_POLICY_H
#define MT2_RECONNECT_POLICY_H
#include "mt2_cod_match.h"
/* Should the connection-keeper page this paired device right now?
 * Yes iff it is OUR device (by class-of-device, tolerant of live service bits — see mt2_cod_match.h)
 * AND it is not already connected. This is the daemon's idempotency invariant in one place:
 * never page a connected device; never touch a co-connected non-matching Apple device. */
static inline int mt2_reconnect_should_page(unsigned cod, int is_connected) {
    return mt2_cod_is_mt2(cod) && !is_connected;
}
#endif
