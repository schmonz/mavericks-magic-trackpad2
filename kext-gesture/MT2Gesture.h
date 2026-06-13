#ifndef MT2GESTURE_H
#define MT2GESTURE_H
#include <IOKit/IOService.h>
#include "amd_shim.h"

/* The transport nub: constructs/drives an AppleMultitouchDevice (see MT2Gesture.cpp)
 * and exposes feedFrame() so its user client (MT2GestureUserClient) can push
 * userspace-decoded MT1 frames into AppleMultitouchDevice::handleTouchFrame. */
class com_schmonz_MT2HIDShell;

class com_schmonz_MT2Gesture : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2Gesture)
    AppleMultitouchDevice *fDevice;
    com_schmonz_MT2HIDShell *fHidShell;   /* in-kernel MT1 HID device under us;
                                             instantiates the started event driver
                                             the actuation wrapper wires to (M5) */
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /* Push one MT1 (0x28) report into the gesture device verbatim. handleTouchFrame
     * enqueues it to the connected AppleMultitouchDeviceUserClient (MultitouchSupport).
     * Returns the device's IOReturn (kIOReturnSuccess, or 0xE00002BC if not yet
     * ready / no client connected, etc.). */
    IOReturn feedFrame(const unsigned char *buf, unsigned int len);
};
#endif
