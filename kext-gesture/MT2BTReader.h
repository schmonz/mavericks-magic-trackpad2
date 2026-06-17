#ifndef MT2BTREADER_H
#define MT2BTREADER_H
#include <IOKit/IOService.h>
#include "touch_model.h"

class IOBluetoothL2CAPChannel;

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

    /* IOBluetoothL2CAPChannel::listenAt callback: (target, channel, length, data). */
    static void incomingData(IOService *target, IOBluetoothL2CAPChannel *channel,
                             unsigned short length, void *data);
};
#endif
