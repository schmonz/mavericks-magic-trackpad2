#ifndef MT2_RECONNECT_POLICY_H
#define MT2_RECONNECT_POLICY_H
#include "mt2_cod_match.h"
/* Should the connection-keeper page this paired device right now?
 * Yes iff it is OUR device (by class-of-device, tolerant of live service bits — see mt2_cod_match.h)
 * AND it is not already connected over BT AND we are NOT currently driving over USB.
 * The daemon's idempotency invariant in one place: never page a connected device; never touch a
 * co-connected non-matching Apple device; and NEVER page BT while on USB.
 * That last gate is load-bearing: the MT2 is single-transport, so a BT page that succeeds while the
 * device streams over USB flips it to Bluetooth and KILLS the USB stream (root-caused 2026-07-20 — the
 * periodic USB cursor stalls were our own keeper paging us off USB). usb_present is the shared presence
 * SM's truth (presence_observer_state(obs) == PRESENCE_USB), not a private flag. */
static inline int mt2_reconnect_should_page(unsigned cod, int is_connected, int usb_present) {
    return mt2_cod_is_mt2(cod) && !is_connected && !usb_present;
}
#endif
