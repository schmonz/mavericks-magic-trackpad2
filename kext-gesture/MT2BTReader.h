#ifndef MT2BTREADER_H
#define MT2BTREADER_H
#include <IOKit/IOService.h>

class IOBluetoothL2CAPChannel;

/* Matches the Magic Trackpad 2's Bluetooth L2CAP channel (its BT-SIG identity:
 * VendorID 76 / ProductID 613 / VendorIDSource 1) with a high IOProbeScore, the way
 * bnbinject made Apple's BNBTrackpadDevice bind it. On start it enables multitouch
 * (0xF1) and registers an incoming-data listener; each interrupt frame is decoded
 * (mt2_bt_decode), re-encoded as an MT1 report (mt1_encode), and fed to the active
 * MT2Gesture nub in-kernel. The BT counterpart of the USB MT2USBClaim + feeder. */
class com_schmonz_MT2BTReader : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2BTReader)
    IOBluetoothL2CAPChannel *fChannel;
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /* IOBluetoothL2CAPChannel::listenAt callback: (target, channel, length, data). */
    static void incomingData(IOService *target, IOBluetoothL2CAPChannel *channel,
                             unsigned short length, void *data);
};
#endif
