#include <sys/sysctl.h>
#include "mavericks_log.h"

/* Runtime log level: 0=off (default), 1=milestones, 2=verbose. Read by MAVERICKS_DLOG. */
int gMavericksLogLevel = 0;

/* debug.mavericks_log — read/write int. CTLFLAG_LOCKED: we don't need the (legacy) sysctl lock held. */
SYSCTL_INT(_debug, OID_AUTO, mavericks_log, CTLFLAG_RW | CTLFLAG_LOCKED,
           &gMavericksLogLevel, 0, "Mavericks Trackpad 2 driver log level (0=off,1=milestones,2=verbose)");

/* debug.mt2_batt — force the published battery % for prefpane UI testing. -1 = off (report the real
 * device value); 0-100 = publish this instead (e.g. 0 to exercise the pane's low-battery /
 * Change-Batteries painting). Read in MT2BTReader's mavericks_publish_battery. */
int gMavericksBattOverride = -1;
SYSCTL_INT(_debug, OID_AUTO, mt2_batt, CTLFLAG_RW | CTLFLAG_LOCKED,
           &gMavericksBattOverride, -1, "MT2 battery percent override for UI testing (-1=off, 0-100=force)");


static bool gMavericksSysctlRegistered = false;

void mavericks_log_register(void) {
    if (!gMavericksSysctlRegistered) {
        sysctl_register_oid(&sysctl__debug_mavericks_log);
        sysctl_register_oid(&sysctl__debug_mt2_batt);
        gMavericksSysctlRegistered = true;
    }
}

void mavericks_log_unregister(void) {
    if (gMavericksSysctlRegistered) {
        sysctl_unregister_oid(&sysctl__debug_mavericks_log);
        sysctl_unregister_oid(&sysctl__debug_mt2_batt);
        gMavericksSysctlRegistered = false;
    }
}
