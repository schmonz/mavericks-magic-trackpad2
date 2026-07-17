#ifndef MT2_COD_MATCH_H
#define MT2_COD_MATCH_H
/* The MT2's Bluetooth Class-of-Device match, in ONE place.
 *
 * The stored/nominal CoD is 0x594 — peripheral major class 5, pointing + digitizer minor 0x25.
 * But the CoD field is 24 bits: bits 0-12 are the DEVICE class (format + minor + major), bits
 * 13-23 are the SERVICE class. The LIVE CoD read from a paired device can carry service bits the
 * stored value doesn't — e.g. 0x2594, which is 0x594 plus the 0x2000 Limited-Discoverable service
 * bit. So an exact `cod == 0x594` silently MISSES the MT2 whenever a service bit is set (observed
 * 2026-07-15: broke USB->BT handoff, "no paired CoD-0x594 MT2 found"). Match on the device-class
 * field only. See docs/mt-stack/open-questions.md "CoD exact-match misses the MT2".
 */
#define MT2_COD_DEVCLASS      0x594u    /* peripheral(5) + pointing + digitizer minor 0x25 */
#define MT2_COD_DEVCLASS_MASK 0x1FFFu   /* CoD bits 0-12 = the device-class field (mask off service bits) */

static inline int mt2_cod_is_mt2(unsigned cod) {
    return (cod & MT2_COD_DEVCLASS_MASK) == MT2_COD_DEVCLASS;
}

#endif
