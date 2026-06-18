/*
 * MT2GestureUserClient - DEBUG/TEST userspace->kernel seam for hands-free on-device
 * testing (restored from f9c814a^). A test tool (tools/synth_tap, tools/synth_feed) runs
 * our decode/session/encode in userspace and pushes each encoded MT1 0x28 report in via
 * IOConnectCallStructMethod(conn, selector=0, bytes, len). externalMethod routes selector
 * 0 to com_schmonz_MT2Gesture::feedFrame -> AppleMultitouchDevice::handleTouchFrame, which
 * passes the buffer through verbatim. Returns benignly (not a panic) if the device is not
 * yet ready. Read path only; the in-kernel readers remain the production input.
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
