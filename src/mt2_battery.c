#include "mt2_battery.h"

/* See mt2_battery.h. The report id (0x90) and the capacity offset (byte[2]) are the RE'd facts
 * (docs/mt-stack/battery-reporting.md); everything else is defensive framing. */
#define MT2_HIDP_DATA_INPUT   0xA1   /* HIDP DATA transaction, Input report type — control-chan wire byte */
#define MT2_BATT_REPORT_ID    0x90
#define MT2_BATT_CAPACITY_OFF 2      /* [id][flags][capacity] */

int mt2_parse_battery_report(const uint8_t *data, size_t len, uint8_t *out_pct)
{
    if (!data || !out_pct) return 0;

    /* Strip the optional 0xA1 HIDP transport byte (present on raw L2CAP control PDUs; absent
     * when a caller already stripped it). */
    if (len > 0 && data[0] == MT2_HIDP_DATA_INPUT) { data++; len--; }

    if (len <= MT2_BATT_CAPACITY_OFF) return 0;      /* need id + flags + capacity */
    if (data[0] != MT2_BATT_REPORT_ID) return 0;     /* not the battery report */

    uint8_t pct = data[MT2_BATT_CAPACITY_OFF];
    if (pct > 100) return 0;                          /* capacity is 0-100; reject garbage */

    *out_pct = pct;
    return 1;
}
