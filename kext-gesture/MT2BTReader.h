#ifndef MT2BTREADER_H
#define MT2BTREADER_H
#include <IOKit/IOService.h>
#include "voodoo_input.h"
#include "genuine_host.h"          /* shared manual-start + ordered-teardown core */

class IOBluetoothL2CAPChannel;
class IOTimerEventSource;

/* Matches the Magic Trackpad 2's Bluetooth L2CAP channel (its BT-SIG identity:
 * VendorID 76 / ProductID 613 / VendorIDSource 1) with a high IOProbeScore — the same
 * identity Apple's stock BNBTrackpadDevice matches. On the control channel it manual-starts a
 * genuine BNBTrackpadDevice on the real L2CAP channel, then interposes a MT2->MT1 translation shim
 * on BNB's interrupt-channel delegate, injects sensor geometry, and defers the 0xF1 multitouch
 * enable — so Apple's own BNB drives the cursor/gestures/prefpane and we only condition the
 * stream. The BT counterpart of the USB transport. */
class com_schmonz_MT2BTReader : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2BTReader)
    IOBluetoothL2CAPChannel *fChannel;
    bool fIsControl;        /* set in-gate: PSM 17 (control) — the channel BNBTrackpadDriver wants */
    IOService *fManualBnb;  /* a genuine BNBTrackpadDevice we instantiate + start directly on this
                               real channel (bypass IOKit matching, which can't be tricked — see
                               findings S2.2c). */
    IOTimerEventSource *fInterposeTimer;  /* polls for BNB's interrupt channel, then installs the shim */
    int fInterposeTries;            /* retry budget for the installer poll */
    int fReEnableCount;             /* Path A: # of post-install 0xF1 re-enables sent (force multitouch mode) */
    gh_host_t fHost;                /* genuine_host lifecycle handle for fManualBnb */
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /* Runs inside the channel's Bluetooth workloop gate (via getCommandGate()->runAction):
     * sends the 0xF1 enable and registers the incoming-data listener. IOBluetoothFamily
     * REQUIREs these calls happen in-gate. arg0 = the com_schmonz_MT2BTReader. */
    static IOReturn setupInGate(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    /* In-gate teardown: clears our listenAt callback so the channel's newDataIn stops
     * dereferencing this (soon-to-be-freed) object — without it, unloading while
     * connected leaves a dangling listener and panics IOBluetoothFamily. arg0 = reader. */
    static IOReturn teardownInGate(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    /* IOTimerEventSource handler: poll gGenuineBnb+0xf0 for BNB's interrupt channel;
     * once present, install the delegate-interpose in that channel's command gate. */
    static void interposeTimerFired(OSObject *owner, IOTimerEventSource *ts);
    /* In the channel's command gate: swap channel+0x110 (delegate callback) to our shim,
     * saving BNB's original. arg0 = the IOBluetoothL2CAPChannel. */
    static IOReturn interposeInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* In-gate: restore BNB's original callback before we tear down. arg0 = channel. */
    static IOReturn restoreInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* Battery: interpose/restore BNB's CONTROL-channel (PSM 17) delegate — separate from the
     * interrupt interpose — so we can sniff GET_REPORT(0x90) responses. arg0 = the control channel. */
    static IOReturn controlInterposeInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    static IOReturn controlRestoreInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* In the CONTROL channel's gate: send a HIDP GET_REPORT(Input, 0x90) battery poll. arg0 = the
     * control reader (self); its fChannel is the PSM-17 control channel. */
    static IOReturn pollBatteryInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* In the CONTROL channel's gate: re-send the 0xF1 multitouch enable. BNB's handleStart
     * resets the device to mouse mode (report 0x02) after our initial enable; re-sending forces
     * it back to multitouch (report 0x31). arg0 = the control reader (self). */
    static IOReturn reEnableInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* B1-drive probe — in the INTERRUPT channel's gate: inject a synthetic 0x60/0x02 report into
     * BNB's own data callback (gOrigCb) so BNB runs createMultitouchHandler and spawns its own
     * AppleMultitouchDevice (S2.16/S2.17). arg0 = the interposed interrupt channel. */
    static IOReturn triggerInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);

    /* BNB geometry: clone the transport's vtable + override
     * getMultitouchReport so BNB's spawned AMD publishes real sensor geometry on its first
     * cacheDeviceProperties (before the MTDevice is born). installBnbGeometry() runs BEFORE the
     * create trigger; removeBnbGeometry() restores the original vtable on stop. */
    void installBnbGeometry(void *transport);
    void removeBnbGeometry(void *transport);
};
#endif
