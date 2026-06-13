/*
 * MT2GestureUserClient - the userspace -> kernel seam for Milestone 4.
 *
 * The userspace feeder (tools/mt2_gesture_feed.c) reads raw MT2 frames, decodes +
 * re-encodes them to the MT1 0x28 report, and pushes each report in via
 * IOConnectCallStructMethod(conn, selector=0, bytes, len). externalMethod routes
 * selector 0 to com_schmonz_MT2Gesture::feedFrame -> AppleMultitouchDevice::
 * handleTouchFrame, which enqueues the bytes to MultitouchSupport's user client.
 *
 * RE finding behind this design: handleTouchFrame passes the buffer through
 * verbatim (enqueueData), so the struct input is exactly the MT1 report and needs
 * no in-kernel translation. It also safely returns 0xE00002BC (not a panic) if the
 * device is not yet ready, so feeding before MultitouchSupport connects is benign.
 */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "MT2Gesture.h"

class com_schmonz_MT2GestureUserClient : public IOUserClient {
    OSDeclareDefaultStructors(com_schmonz_MT2GestureUserClient)
    com_schmonz_MT2Gesture *fOwner;
public:
    virtual bool start(IOService *provider) override;
    virtual IOReturn clientClose(void) override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(com_schmonz_MT2GestureUserClient, IOUserClient)

bool com_schmonz_MT2GestureUserClient::start(IOService *provider) {
    fOwner = OSDynamicCast(com_schmonz_MT2Gesture, provider);
    if (!fOwner) {
        IOLog("MT2GestureUserClient: provider is not com_schmonz_MT2Gesture\n");
        return false;
    }
    if (!IOUserClient::start(provider)) {
        return false;
    }
    IOLog("MT2GestureUserClient: started\n");
    return true;
}

IOReturn com_schmonz_MT2GestureUserClient::clientClose(void) {
    IOLog("MT2GestureUserClient: clientClose\n");
    if (!isInactive()) {
        terminate();
    }
    return kIOReturnSuccess;
}

IOReturn com_schmonz_MT2GestureUserClient::externalMethod(
        uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void)dispatch; (void)target; (void)reference;
    if (selector != 0) {
        return kIOReturnUnsupported;
    }
    if (!fOwner) {
        return kIOReturnNotAttached;
    }
    if (!args || !args->structureInput || args->structureInputSize == 0) {
        return kIOReturnBadArgument;
    }
    return fOwner->feedFrame((const unsigned char *)args->structureInput,
                             (unsigned int)args->structureInputSize);
}
