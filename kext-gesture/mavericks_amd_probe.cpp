#include <sys/sysctl.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSDictionary.h>
#include "mavericks_amd_probe.h"
#include "MavericksAMDTerminal.h"

class com_schmonz_MavericksVoodooInputHost;
extern com_schmonz_MavericksVoodooInputHost *gActiveMavericksVoodooInputHost;   /* engine nub (defined in MavericksVoodooInputHost.cpp) */

static mavericks_amd_terminal_ctx *gProbeCtx = 0;
static int gProbeVal = 0;

static int probe_amd_count(void) {
    OSDictionary *m = IOService::serviceMatching("AppleMultitouchDevice");
    if (!m) return -1;
    OSIterator *it = IOService::getMatchingServices(m);
    m->release();
    if (!it) return 0;
    int n = 0;
    while (it->getNextObject()) n++;
    it->release();
    return n;
}

static void probe_run(int n) {
    IOService *nub = (IOService *)gActiveMavericksVoodooInputHost;
    if (!nub) { IOLog("mavericks_amd_probe: no engine nub; ignoring\n"); return; }
    if (n == 1) {
        if (gProbeCtx) { IOLog("mavericks_amd_probe: already up\n"); return; }
        int before = probe_amd_count();
        gProbeCtx = mavericks_amd_terminal_build(nub, MAVERICKS_AMD_TERMINAL_XPORT_BT);   /* oracle: transport is arbitrary */
        IOLog("mavericks_amd_probe: BUILD ctx=%p amd_count %d->%d\n", gProbeCtx, before, probe_amd_count());
    } else if (n == 0) {
        if (!gProbeCtx) { IOLog("mavericks_amd_probe: nothing to tear down\n"); return; }
        int before = probe_amd_count();
        mavericks_amd_terminal_teardown(nub, gProbeCtx);
        gProbeCtx = 0;
        IOLog("mavericks_amd_probe: TEARDOWN amd_count %d->%d\n", before, probe_amd_count());
    } else { /* n >= 2: churn, with adoption settle each cycle */
        int base = probe_amd_count();
        for (int i = 0; i < n; i++) {
            mavericks_amd_terminal_ctx *c = mavericks_amd_terminal_build(nub, MAVERICKS_AMD_TERMINAL_XPORT_BT);
            IOSleep(300);   /* let hidd/AppleMultitouchDriver adopt -> frames-client forms */
            mavericks_amd_terminal_teardown(nub, c);
            IOSleep(100);
        }
        int fin = probe_amd_count();
        IOLog("mavericks_amd_probe: CHURN n=%d final amd_count=%d base=%d -> %s\n",
              n, fin, base, (fin == base) ? "PASS" : "LEAK");
    }
}

static int mavericks_amd_probe_sysctl SYSCTL_HANDLER_ARGS {
    int err = sysctl_handle_int(oidp, &gProbeVal, 0, req);
    if (err || !req->newptr) return err;
    probe_run(gProbeVal);
    return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, mavericks_amd_probe, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
            0, 0, mavericks_amd_probe_sysctl, "I",
            "MT2 fabricated-AMD oracle: 1=build, 0=teardown, N>=2=churn N cycles");

static bool gRegistered = false;
void mavericks_amd_probe_register(void)   { if (!gRegistered) { sysctl_register_oid(&sysctl__debug_mavericks_amd_probe); gRegistered = true; } }
void mavericks_amd_probe_unregister(void) { if (gRegistered) { sysctl_unregister_oid(&sysctl__debug_mavericks_amd_probe); gRegistered = false; } }
