#include "../src/mt2_battery.h"
#include "test.h"

/* mt2_parse_battery_report: the shared pure decode of the Apple Power-Device report id 0x90
 * that both BT shims feed. Oracle for the RE'd fact "capacity = byte[2], 0-100, optional 0xA1
 * transport byte" (docs/mt-stack/battery-reporting.md). A regression here silently breaks the
 * prefpane battery number, which is otherwise only observable on-device. */
static void run_tests(void) {
    uint8_t pct = 0xEE;

    /* Raw control-channel PDU with the 0xA1 HIDP DATA/Input transport byte (BT wire form). */
    uint8_t raw_full[] = { 0xA1, 0x90, 0x00, 0x64 };     /* on battery, 100% */
    CHECK_EQ(mt2_parse_battery_report(raw_full, sizeof(raw_full), &pct), 1);
    CHECK_EQ(pct, 100);

    /* Charging flag (byte[1]=0x05) must not affect the capacity read. */
    pct = 0xEE;
    uint8_t raw_charging[] = { 0xA1, 0x90, 0x05, 0x64 };
    CHECK_EQ(mt2_parse_battery_report(raw_charging, sizeof(raw_charging), &pct), 1);
    CHECK_EQ(pct, 100);

    /* Already-stripped form (no 0xA1) — the interrupt shim passes this. */
    pct = 0xEE;
    uint8_t stripped[] = { 0x90, 0x00, 0x2A };           /* 42% */
    CHECK_EQ(mt2_parse_battery_report(stripped, sizeof(stripped), &pct), 1);
    CHECK_EQ(pct, 42);

    /* Boundary: 0% is valid. */
    pct = 0xEE;
    uint8_t zero[] = { 0x90, 0x00, 0x00 };
    CHECK_EQ(mt2_parse_battery_report(zero, sizeof(zero), &pct), 1);
    CHECK_EQ(pct, 0);

    /* Reject: capacity > 100 (garbage / not really a capacity byte). out_pct untouched. */
    pct = 0xEE;
    uint8_t over[] = { 0x90, 0x00, 0x65 };               /* 101 */
    CHECK_EQ(mt2_parse_battery_report(over, sizeof(over), &pct), 0);
    CHECK_EQ(pct, 0xEE);

    /* Reject: a different report id (e.g. the 0x31 multitouch stream, or 0x28 MT1). */
    pct = 0xEE;
    uint8_t mt[] = { 0xA1, 0x31, 0x48, 0x83, 0x83 };
    CHECK_EQ(mt2_parse_battery_report(mt, sizeof(mt), &pct), 0);
    CHECK_EQ(pct, 0xEE);

    /* Reject: too short (id + flags but no capacity byte). */
    pct = 0xEE;
    uint8_t shortf[] = { 0x90, 0x00 };
    CHECK_EQ(mt2_parse_battery_report(shortf, sizeof(shortf), &pct), 0);
    CHECK_EQ(pct, 0xEE);

    /* Reject: bare 0xA1 with nothing after it, and empty/NULL inputs (no OOB read/crash). */
    pct = 0xEE;
    uint8_t just_a1[] = { 0xA1 };
    CHECK_EQ(mt2_parse_battery_report(just_a1, sizeof(just_a1), &pct), 0);
    CHECK_EQ(mt2_parse_battery_report(0, 4, &pct), 0);
    CHECK_EQ(mt2_parse_battery_report(stripped, 0, &pct), 0);
    CHECK_EQ(pct, 0xEE);
}
TEST_MAIN()
