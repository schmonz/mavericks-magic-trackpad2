#ifndef MT2BTREADER_H
#define MT2BTREADER_H
#include <IOKit/IOService.h>
#include "mavericks_frame.h"

class IOBluetoothL2CAPChannel;
class IOTimerEventSource;

/* Matches the Magic Trackpad 2's Bluetooth L2CAP channel (its BT-SIG identity:
 * VendorID 76 / ProductID 613 / VendorIDSource 1) with a high IOProbeScore — the same
 * identity Apple's stock BNBTrackpadDevice matches. On the interrupt channel it registers
 * itself as the L2CAP delegate (listenAt -> incomingData) and builds a fabricated AMD;
 * on the control channel it owns the 0xF1 multitouch enable + battery poll. No
 * BNBTrackpadDevice is ever started. The BT counterpart of the USB transport. */
class MT2BTReader : public IOService {
    OSDeclareDefaultStructors(MT2BTReader)
    IOBluetoothL2CAPChannel *fChannel;
    bool fIsControl;        /* set in-gate: PSM 17 (control) — owns the 0xF1 enable + battery poll */
    IOTimerEventSource *fInterposeTimer;  /* fires the deferred 0xF1 enable + battery poll */
    int fInterposeTries;            /* (unused; kept for resetTransportState symmetry) */
    int fReEnableCount;             /* # of 0xF1 re-enables sent before first frame */
    bool fStreaming;                /* true after incomingData delivers the first real multitouch frame */
    IOService *fMux;                /* the bound VoodooInput mux (found lazily by VOODOO_INPUT_IDENTIFIER) */
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /* Runs inside the channel's Bluetooth workloop gate (via getCommandGate()->runAction):
     * sends the 0xF1 enable and registers the incoming-data listener. IOBluetoothFamily
     * REQUIREs these calls happen in-gate. arg0 = the MT2BTReader. */
    static IOReturn setupInGate(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    /* In-gate teardown: clears our listenAt callback so the channel's newDataIn stops
     * dereferencing this (soon-to-be-freed) object — without it, unloading while
     * connected leaves a dangling listener and panics IOBluetoothFamily. arg0 = reader. */
    static IOReturn teardownInGate(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    /* Interrupt-channel data delegate: decode 0x31 -> submitFrame. We own this delegate directly (no
     * BNB) — the genuine path could not (BNB's listenAt would 0xe00002bc -> panic); without BNB it's clean. */
    static void incomingData(IOService *target, IOBluetoothL2CAPChannel *channel,
                             unsigned short length, void *data);

    /* Control-channel accept delegate (INERT — ignores every arg). Its only purpose is to be the
     * listenAt callback that "accepts" the control channel: genuine IOBluetoothHIDDriver listenAt's
     * control then waitForChannelState(OPEN), and that acceptance is what provokes the device to open
     * its (device-initiated) PSM 19 interrupt channel. Because it derefs nothing, it stays safe even if
     * the battery interpose's restore leaves the channel pointing at a freed reader. Battery sniffing is
     * still done by bt_control_shim (the interpose), not here. See reference.md "BT connect handshake". */
    static void controlData(IOService *target, IOBluetoothL2CAPChannel *channel,
                            unsigned short length, void *data);

    /* IOTimerEventSource handler: deferred 0xF1 multitouch enable (retried until first frame),
     * then steady-state battery polling. */
    static void interposeTimerFired(OSObject *owner, IOTimerEventSource *ts);

    /* Battery: interpose/restore the CONTROL-channel (PSM 17) delegate so we can sniff
     * GET_REPORT(0x90) responses. arg0 = the control channel. */
    static IOReturn controlInterposeInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    static IOReturn controlRestoreInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* In the CONTROL channel's gate: send a HIDP GET_REPORT(Input, 0x90) battery poll. arg0 = the
     * control reader (self); its fChannel is the PSM-17 control channel. */
    static IOReturn pollBatteryInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);
    /* In the CONTROL channel's gate: re-send the 0xF1 multitouch enable. arg0 = the control reader. */
    static IOReturn reEnableInGate(OSObject *owner, void *arg0, void *a1, void *a2, void *a3);

    /* Route a rename to the device: SET_REPORT(Feature, 0x55, [name]) over the control channel, so the
     * name FOLLOWS the device. The satellite excludes IOBluetoothHIDDriver, so the IOHIDManager path can't
     * reach the real MT2's feature reports — MavericksHIDShell::setReport funnels 0x55 here instead. Called
     * on IOHIDManager's thread; marshals the L2CAP send onto the control channel's BT workloop via
     * getCommandGate()->runAction (same discipline as reEnableInGate/pollBatteryInGate). Best-effort:
     * returns kIOReturnNoDevice when there is no BT control channel (USB or disconnected). */
    static IOReturn writeDeviceName(const uint8_t *name, unsigned int len);
    /* In the CONTROL channel's gate: build [0x53 SET_REPORT|Feature][0x55][name] and sendTo. Guards a
     * torn-down channel (isInactive). arg0 = the control channel, arg1 = name bytes, arg2 = length. */
    static IOReturn sendNameInGate(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

    /* Read the on-device name back: send GET_REPORT(Feature, 0x55) on the control channel and wait
     * (bounded IOSleep) for the device's DATA|Feature reply, which bt_control_shim captures into the
     * gNameResp buffer. Fills out[0..] with the name bytes (no report-id/transport byte) + *outLen. Returns
     * kIOReturnTimeout if no reply lands, kIOReturnNoDevice off-BT. Called on IOHIDManager's getReport
     * thread (may IOSleep); restores the round-trip verification the satellite broke. */
    static IOReturn readDeviceName(uint8_t *out, unsigned int *outLen, unsigned int maxLen);
    /* In the CONTROL channel's gate: send [0x43 GET_REPORT|Feature][0x55]. arg0 = the control channel. */
    static IOReturn sendNameGetInGate(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);

private:
    /* start() split into named steps (extract-method only; call order preserved). */
    void resetTransportState();       /* zero the per-connection fields */
    bool marshalSetupInGate();        /* run setupInGate on the channel gate; false = no gate */
    void armInterposeTimer();         /* arm the deferred 0xF1 enable + battery-poll timer */
};
#endif
