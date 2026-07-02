#ifndef MT2_BATTERY_H
#define MT2_BATTERY_H
#include <stdint.h>
#include <stddef.h>

/* Parse an Apple Power-Device battery report (report id 0x90) as the MT2 emits it on both
 * transports: [0x90 id][status flags][capacity 0-100] (Usage Page 0x84 Power Device / 0x85
 * Battery System; capacity = Usage 0x65). A leading 0xA1 HIDP DATA/Input transport byte is
 * accepted and stripped (so raw L2CAP control-channel bytes and already-stripped reports both
 * parse). On a valid battery report, writes the capacity (0-100) to *out_pct and returns 1;
 * otherwise leaves *out_pct untouched and returns 0. Pure + side-effect-free so it is
 * host-testable; the kext wraps it with the on-change publish. Verified live on USB + BT
 * (`90 05 64` = 100% charging, `90 00 64` = 100% on battery). */
int mt2_parse_battery_report(const uint8_t *data, size_t len, uint8_t *out_pct);

#endif
