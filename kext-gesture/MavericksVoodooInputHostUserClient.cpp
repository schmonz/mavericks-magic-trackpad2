/*
 * MavericksVoodooInputHostUserClient - DEBUG/TEST userspace->kernel seam for hands-free on-device
 * testing (restored from f9c814a^). A test tool (tools/synth_tap, tools/synth_feed) runs
 * our decode/session/encode in userspace and pushes each encoded MT1 0x28 report in via
 * IOConnectCallStructMethod(conn, selector=0, bytes, len). externalMethod routes selector
 * 0 to MavericksVoodooInputHost::feedFrame -> AppleMultitouchDevice::handleTouchFrame, which
 * passes the buffer through verbatim. Returns benignly (not a panic) if the device is not
 * yet ready. Read path only; the in-kernel readers remain the production input.
 */
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "MavericksVoodooInputHost.h"

class MavericksVoodooInputHostUserClient : public IOUserClient {
    OSDeclareDefaultStructors(MavericksVoodooInputHostUserClient)
    MavericksVoodooInputHost *fOwner;
public:
    virtual bool start(IOService *provider) override;
    virtual IOReturn clientClose(void) override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(MavericksVoodooInputHostUserClient, IOUserClient)

bool MavericksVoodooInputHostUserClient::start(IOService *provider) {
    fOwner = OSDynamicCast(MavericksVoodooInputHost, provider);
    if (!fOwner) {
        IOLog("MavericksVoodooInputHostUserClient: provider is not MavericksVoodooInputHost\n");
        return false;
    }
    if (!IOUserClient::start(provider)) {
        return false;
    }
    IOLog("MavericksVoodooInputHostUserClient: started\n");
    return true;
}

IOReturn MavericksVoodooInputHostUserClient::clientClose(void) {
    IOLog("MavericksVoodooInputHostUserClient: clientClose\n");
    if (!isInactive()) {
        terminate();
    }
    return kIOReturnSuccess;
}

IOReturn MavericksVoodooInputHostUserClient::externalMethod(
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
