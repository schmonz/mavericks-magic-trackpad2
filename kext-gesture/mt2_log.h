#ifndef MT2_LOG_H
#define MT2_LOG_H
/* Runtime-gated diagnostics. Default OFF. Toggle live, no rebuild/reboot:
 *   sudo sysctl debug.mt2_log=1   # milestones
 *   sudo sysctl debug.mt2_log=2   # verbose (per-report geometry, per-edge clicks)
 *   sudo sysctl debug.mt2_log=0   # silence
 * Errors/failures use IOLog directly (always visible); MT2_DLOG is for non-error observability.
 * The sysctl is registered in com_schmonz_MT2Gesture::start and unregistered in stop (single
 * instance), so the oid never outlives the kext. */

#include <IOKit/IOLib.h>

extern int gMT2LogLevel;

/* debug.mt2_batt: force the published battery % for prefpane UI testing (-1=off, 0-100=force).
 * See mt2_log.cpp; consumed by MT2BTReader's mt2_publish_battery. */
extern int gMT2BattOverride;

/* debug.mt2_bt_synth: 0 = deliver MT2-BT frames to BNB's genuine AMD (default); 1 = deliver to a
 * fabricated AMD (A/B experiment). Read at connectionEstablished by MT2BTReader. */
extern int gMT2BtSynth;

#define MT2_DLOG(lvl, fmt, ...) \
    do { if (gMT2LogLevel >= (lvl)) IOLog("MT2: " fmt "\n", ##__VA_ARGS__); } while (0)

void mt2_log_register(void);    /* idempotent */
void mt2_log_unregister(void);  /* idempotent */

#endif /* MT2_LOG_H */
