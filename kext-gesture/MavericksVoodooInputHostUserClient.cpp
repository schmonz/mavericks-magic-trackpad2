/*
 * MavericksVoodooInputHostUserClient - DEBUG/TEST userspace->kernel seam for hands-free on-device
 * testing (restored from f9c814a^). A test tool (tools/synth_tap, tools/synth_feed) runs
 * our decode/session/encode in userspace and pushes each encoded MT1 0x28 report in via
 * IOConnectCallStructMethod(conn, selector=0, bytes, len). externalMethod routes selector
 * 0 to com_schmonz_MavericksVoodooInputHost::feedFrame -> AppleMultitouchDevice::handleTouchFrame, which
 * passes the buffer through verbatim. Returns benignly (not a panic) if the device is not
 * yet ready. Read path only; the in-kernel readers remain the production input.
 */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "MavericksVoodooInputHost.h"

class com_schmonz_MavericksVoodooInputHostUserClient : public IOUserClient {
    OSDeclareDefaultStructors(com_schmonz_MavericksVoodooInputHostUserClient)
    com_schmonz_MavericksVoodooInputHost *fOwner;
public:
    virtual bool start(IOService *provider) override;
    virtual IOReturn clientClose(void) override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(com_schmonz_MavericksVoodooInputHostUserClient, IOUserClient)

bool com_schmonz_MavericksVoodooInputHostUserClient::start(IOService *provider) {
    fOwner = OSDynamicCast(com_schmonz_MavericksVoodooInputHost, provider);
    if (!fOwner) {
        IOLog("MavericksVoodooInputHostUserClient: provider is not com_schmonz_MavericksVoodooInputHost\n");
        return false;
    }
    if (!IOUserClient::start(provider)) {
        return false;
    }
    IOLog("MavericksVoodooInputHostUserClient: started\n");
    return true;
}

IOReturn com_schmonz_MavericksVoodooInputHostUserClient::clientClose(void) {
    IOLog("MavericksVoodooInputHostUserClient: clientClose\n");
    if (!isInactive()) {
        terminate();
    }
    return kIOReturnSuccess;
}

IOReturn com_schmonz_MavericksVoodooInputHostUserClient::externalMethod(
        uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void)dispatch; (void)target; (void)reference;
    if (!fOwner) {
        return kIOReturnNotAttached;
    }
    switch (selector) {
    case 0:   /* pre-encoded feedFrame: push already-encoded 0x28 bytes through the active transport */
        if (!args || !args->structureInput || args->structureInputSize == 0) {
            return kIOReturnBadArgument;
        }
        return fOwner->feedFrame((const unsigned char *)args->structureInput,
                                 (unsigned int)args->structureInputSize);
    default:
        return kIOReturnBadArgument;
    }
}
