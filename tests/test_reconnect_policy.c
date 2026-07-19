/* The connection-keeper's page decision. Invariant: page a device iff it is OURS (by class,
 * tolerant of live service bits) AND not already connected — never page a connected device,
 * never touch a co-connected non-matching Apple device. Kext-free, pure logic. */
#include "mt2_reconnect_policy.h"
#include <stdio.h>

static int fails = 0;
static void check(const char *name, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d want %d\n", name, got, want); fails++; }
}

int main(void) {
    check("matched + disconnected -> page",     mt2_reconnect_should_page(0x594, 0), 1);
    check("matched + connected -> skip",        mt2_reconnect_should_page(0x594, 1), 0);
    check("service-bit + disconnected -> page", mt2_reconnect_should_page(0x2594, 0), 1);  /* live CoD w/ 0x2000 */
    check("service-bit + connected -> skip",    mt2_reconnect_should_page(0x2594, 1), 0);
    check("non-match + disconnected -> skip",   mt2_reconnect_should_page(0x580, 0), 0);   /* mouse */
    check("non-match + connected -> skip",      mt2_reconnect_should_page(0x580, 1), 0);
    if (fails) { printf("%d reconnect-policy check(s) failed\n", fails); return 1; }
    printf("all reconnect-policy checks passed\n");
    return 0;
}
