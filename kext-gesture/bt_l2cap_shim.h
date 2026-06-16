#ifndef BT_L2CAP_SHIM_H
#define BT_L2CAP_SHIM_H
#include <IOKit/IOService.h>

/* Minimal redeclaration of IOBluetoothL2CAPChannel so we can call its exported,
 * non-virtual kernel methods by symbol — the same technique amd_shim.h uses for
 * AppleMultitouchDevice. We never construct or OSDynamicCast to this type; we receive
 * the real channel as our IOService provider and reinterpret_cast it. The real class is
 * a single-inheritance IOService descendant (IOService subobject at offset 0), so the
 * cast needs no pointer adjustment. These signatures are matched byte-for-byte to the
 * mangled symbols IOBluetoothFamily exports (verified against the 10.9 kext), so they
 * resolve at load via the OSBundleLibraries dependency on com.apple.iokit.IOBluetoothFamily:
 *
 *   __ZN23IOBluetoothL2CAPChannel8listenAtEP9IOServicePFvS1_PS_tPvE
 *   __ZN23IOBluetoothL2CAPChannel6sendToEPvtPFvP9IOServicePS_iyyES2_yy
 *   __ZN23IOBluetoothL2CAPChannel13channelIsOpenEv
 *   __ZN23IOBluetoothL2CAPChannel14setOutgoingMTUEt
 *   __ZN23IOBluetoothL2CAPChannel6getPSMEv
 */
class IOBluetoothL2CAPChannel : public IOService {
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
    IOReturn setOutgoingMTU(unsigned short mtu);
    unsigned short getPSM();
};

#endif
