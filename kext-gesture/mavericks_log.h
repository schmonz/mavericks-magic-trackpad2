#ifndef MAVERICKS_LOG_H
#define MAVERICKS_LOG_H
/* Runtime-gated diagnostics. Default OFF. Toggle live, no rebuild/reboot:
 *   sudo sysctl debug.mavericks_log=1   # milestones
 *   sudo sysctl debug.mavericks_log=2   # verbose (per-report geometry, per-edge clicks)
 *   sudo sysctl debug.mavericks_log=0   # silence
 * Errors/failures use IOLog directly (always visible); MAVERICKS_DLOG is for non-error observability.
 * The sysctl is registered in com_schmonz_MavericksVoodooInputHost::start and unregistered in stop (single
 * instance), so the oid never outlives the kext. */

#include <IOKit/IOLib.h>

extern int gMavericksLogLevel;

/* debug.mt2_batt: force the published battery % for prefpane UI testing (-1=off, 0-100=force).
 * See mavericks_log.cpp; consumed by MT2BTReader's mt2_publish_battery. */
extern int gMT2BattOverride;


#define MAVERICKS_DLOG(lvl, fmt, ...) \
    do { if (gMavericksLogLevel >= (lvl)) IOLog("MT2: " fmt "\n", ##__VA_ARGS__); } while (0)

void mavericks_log_register(void);    /* idempotent */
void mavericks_log_unregister(void);  /* idempotent */

#endif /* MAVERICKS_LOG_H */
