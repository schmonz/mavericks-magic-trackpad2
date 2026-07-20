#ifndef BT_L2CAP_SHIM_H
#define BT_L2CAP_SHIM_H
#include <IOKit/IOService.h>

class IOCommandGate;

/* Minimal redeclaration of IOBluetoothL2CAPChannel (and its IOBluetoothObject base) so
 * we can call their exported, non-virtual kernel methods by symbol — the same technique
 * amd_shim.h uses for AppleMultitouchDevice. We never construct or OSDynamicCast to these
 * types; we receive the real channel as our IOService provider and reinterpret_cast it.
 * The real class is a single-inheritance IOService descendant (IOService subobject at
 * offset 0), so the cast needs no pointer adjustment. Signatures match byte-for-byte the
 * mangled symbols IOBluetoothFamily exports (verified against the 10.9 kext), so they
 * resolve at load via the OSBundleLibraries dependency on com.apple.iokit.IOBluetoothFamily:
 *
 *   __ZNK17IOBluetoothObject14getCommandGateEv
 *   __ZN23IOBluetoothL2CAPChannel8listenAtEP9IOServicePFvS1_PS_tPvE
 *   __ZN23IOBluetoothL2CAPChannel6sendToEPvtPFvP9IOServicePS_iyyES2_yy
 *   __ZN23IOBluetoothL2CAPChannel13channelIsOpenEv
 *   __ZN23IOBluetoothL2CAPChannel14setOutgoingMTUEt
 *   __ZN23IOBluetoothL2CAPChannel6getPSMEv
 *   __ZN23IOBluetoothL2CAPChannel19waitForChannelStateE28IOBluetoothL2CAPChannelState
 */

/* L2CAP channel state passed to waitForChannelState(). The enum's NAME must be exactly
 * IOBluetoothL2CAPChannelState so waitForChannelState below mangles to the exported symbol
 * (…19waitForChannelStateE28IOBluetoothL2CAPChannelState). Only OPEN (= MAVERICKS_L2CAP_STATE_OPEN
 * = 4) is used. */
enum IOBluetoothL2CAPChannelState { kIOBluetoothL2CAPChannelStateOpen = 4 };

class IOBluetoothObject : public IOService {
public:
    /* The command gate guarding this object's Bluetooth workloop. IOBluetoothFamily
     * REQUIREs that all IOBluetoothObject calls (sendTo etc.) run inside this gate
     * (mWorkLoop->inGate()); runAction() on it enters the gate from our start() thread. */
    IOCommandGate *getCommandGate() const;
};

class IOBluetoothL2CAPChannel : public IOBluetoothObject {
public:
    /* Incoming-data registration. The callback fires for each L2CAP frame received on
     * this channel: cb(target, channel, length, dataPtr). This is exactly the model
     * IOBluetoothHIDDriver uses (its incomingInterruptDataCallback has this signature).
     * Returns IOReturn (mangling ignores return type). */
    IOReturn listenAt(IOService *target,
                      void (*cb)(IOService *target, IOBluetoothL2CAPChannel *channel,
                                 unsigned short length, void *data));

    /* Asynchronous write of len bytes from data. completion (may be NULL) fires when the
     * transmit finishes; refcons are passed back to it. Used to send the 0xF1 enable. */
    IOReturn sendTo(void *data, unsigned short length,
                    void (*completion)(IOService *target, IOBluetoothL2CAPChannel *channel,
                                       int status, unsigned long long r1, unsigned long long r2),
                    IOService *target, unsigned long long refcon1, unsigned long long refcon2);

    bool channelIsOpen();

    /* Block (bounded internally, ~5 s) until the channel reaches `state`. The genuine
     * IOBluetoothHIDDriver calls this on the control channel, then the interrupt channel,
     * as the "correct acceptance" that provokes the device to open its (device-initiated)
     * PSM 19 interrupt channel. Omitting it is the ~1 s connect-flap where PSM 19 never
     * opens. See docs/mt-stack/reference.md "BT connect handshake". */
    IOReturn waitForChannelState(IOBluetoothL2CAPChannelState state);

    IOReturn setOutgoingMTU(unsigned short mtu);
    unsigned short getPSM();
};

#endif
