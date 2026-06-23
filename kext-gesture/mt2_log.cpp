#include <sys/sysctl.h>
#include "mt2_log.h"

/* Runtime log level: 0=off (default), 1=milestones, 2=verbose. Read by MT2_DLOG. */
int gMT2LogLevel = 0;

/* debug.mt2_log — read/write int. CTLFLAG_LOCKED: we don't need the (legacy) sysctl lock held. */
SYSCTL_INT(_debug, OID_AUTO, mt2_log, CTLFLAG_RW | CTLFLAG_LOCKED,
           &gMT2LogLevel, 0, "Mavericks Trackpad 2 driver log level (0=off,1=milestones,2=verbose)");

static bool gMT2SysctlRegistered = false;

void mt2_log_register(void) {
    if (!gMT2SysctlRegistered) {
        sysctl_register_oid(&sysctl__debug_mt2_log);
        gMT2SysctlRegistered = true;
    }
}

void mt2_log_unregister(void) {
    if (gMT2SysctlRegistered) {
        sysctl_unregister_oid(&sysctl__debug_mt2_log);
        gMT2SysctlRegistered = false;
    }
}
