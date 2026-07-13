#include <sys/sysctl.h>
#include "mt2_log.h"

/* Runtime log level: 0=off (default), 1=milestones, 2=verbose. Read by MT2_DLOG. */
int gMT2LogLevel = 0;

/* debug.mt2_log — read/write int. CTLFLAG_LOCKED: we don't need the (legacy) sysctl lock held. */
SYSCTL_INT(_debug, OID_AUTO, mt2_log, CTLFLAG_RW | CTLFLAG_LOCKED,
           &gMT2LogLevel, 0, "Mavericks Trackpad 2 driver log level (0=off,1=milestones,2=verbose)");

/* debug.mt2_batt — force the published battery % for prefpane UI testing. -1 = off (report the real
 * device value); 0-100 = publish this instead (e.g. 0 to exercise the pane's low-battery /
 * Change-Batteries painting). Read in MT2BTReader's mt2_publish_battery. */
int gMT2BattOverride = -1;
SYSCTL_INT(_debug, OID_AUTO, mt2_batt, CTLFLAG_RW | CTLFLAG_LOCKED,
           &gMT2BattOverride, -1, "MT2 battery percent override for UI testing (-1=off, 0-100=force)");

/* debug.mt2_bt_synth — A/B the MT2-BT terminal. 0 = deliver to BNB's genuine AMD (default,
 * byte-unchanged); 1 = deliver to a fabricated AMD. Read at connectionEstablished by MT2BTReader. */
int gMT2BtSynth = 0;
SYSCTL_INT(_debug, OID_AUTO, mt2_bt_synth, CTLFLAG_RW | CTLFLAG_LOCKED,
           &gMT2BtSynth, 0, "MT2 BT terminal: 0=genuine BNB AMD (default), 1=fabricated AMD (A/B)");

static bool gMT2SysctlRegistered = false;

void mt2_log_register(void) {
    if (!gMT2SysctlRegistered) {
        sysctl_register_oid(&sysctl__debug_mt2_log);
        sysctl_register_oid(&sysctl__debug_mt2_batt);
        sysctl_register_oid(&sysctl__debug_mt2_bt_synth);
        gMT2SysctlRegistered = true;
    }
}

void mt2_log_unregister(void) {
    if (gMT2SysctlRegistered) {
        sysctl_unregister_oid(&sysctl__debug_mt2_log);
        sysctl_unregister_oid(&sysctl__debug_mt2_batt);
        sysctl_unregister_oid(&sysctl__debug_mt2_bt_synth);
        gMT2SysctlRegistered = false;
    }
}
