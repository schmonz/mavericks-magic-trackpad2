/* CoD match predicate: the MT2 must be recognized whether or not the live CoD carries
 * service-class bits. Guards the 2026-07-15 regression where exact == 0x594 missed 0x2594
 * (Limited-Discoverable bit) and broke USB->BT handoff. Kext-free, pure logic. */
#include "mt2_cod_match.h"
#include <stdio.h>

static int fails = 0;
static void check(const char *name, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d want %d\n", name, got, want); fails++; }
}

int main(void) {
    /* The stored/nominal value matches. */
    check("bare 0x594", mt2_cod_is_mt2(0x594), 1);
    /* The live value with the Limited-Discoverable service bit (0x2000) — the actual bug. */
    check("0x2594 (discoverable bit)", mt2_cod_is_mt2(0x2594), 1);
    /* Any combination of service-class bits (13-23) must still match. */
    check("0x594 | all service bits", mt2_cod_is_mt2(0x594u | 0xFFE000u), 1);
    /* A different device class must NOT match, even sharing service bits. */
    check("0x2595 (minor differs)", mt2_cod_is_mt2(0x2595), 0);
    check("0x2500 (minor cleared)", mt2_cod_is_mt2(0x2500), 0);
    check("mouse 0x580", mt2_cod_is_mt2(0x580), 0);
    check("zero", mt2_cod_is_mt2(0), 0);

    if (fails) { printf("%d cod-match check(s) failed\n", fails); return 1; }
    printf("all cod-match checks passed\n");
    return 0;
}
