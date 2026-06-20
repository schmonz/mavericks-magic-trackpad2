#ifndef MT2BTREADER_H
#define MT2BTREADER_H
#include <IOKit/IOService.h>
#include "touch_model.h"

class IOBluetoothL2CAPChannel;
class IOTimerEventSource;

/* Matches the Magic Trackpad 2's Bluetooth L2CAP channel (its BT-SIG identity:
 * VendorID 76 / ProductID 613 / VendorIDSource 1) with a high IOProbeScore, the way
 * bnbinject made Apple's BNBTrackpadDevice bind it. On start it enables multitouch
 * (0xF1) and registers an incoming-data listener; each interrupt frame is decoded
 * (mt2_bt_decode) and pushed to the active MT2Gesture nub via submitFrame
 * (MT2_EVENT_DRIVEN) — the shared session owns settle/lift-drop/decel/click. The BT
 * counterpart of the USB transport. */
class com_schmonz_MT2BTReader : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2BTReader)
    IOBluetoothL2CAPChannel *fChannel;
    bool fIsControl;        /* set in-gate: PSM 17 (control) — the channel BNBTrackpadDriver wants */
    IOService *fManualBnb;  /* Path A (kGenuinePathA): a genuine BNBTrackpadDevice we instantiate +
                               start directly on this real channel (bypass IOKit matching, which
                               can't be tricked — see findings S2.2c). */
    IOTimerEventSource *fInterposeTimer;  /* polls for BNB's interrupt channel, then installs the shim */
    int fInterposeTries;            /* retry budget for the installer poll */
    int fReEnableCount;             /* Path A: # of post-install 0xF1 re-enables sent (force multitouch mode) */
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
    /* In the CONTROL channel's gate: re-send the 0xF1 multitouch enable. BNB's handleStart
     * resets the device to mouse mode (report 0x02) after our initial enable; re-sending forces
     * it back to multitouch (report 0x31). arg0 = the control reader (self). */
    static IOReturn reEnableInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);

    /* IOBluetoothL2CAPChannel::listenAt callback: (target, channel, length, data). */
    static void incomingData(IOService *target, IOBluetoothL2CAPChannel *channel,
                             unsigned short length, void *data);
};
#endif
