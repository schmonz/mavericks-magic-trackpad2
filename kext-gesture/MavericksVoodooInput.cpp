#include "MavericksVoodooInput.h"
#include "mavericks_log.h"              // MAVERICKS_DLOG
#include "voodoo_wire.h"               // VoodooInputEvent + kIOMessage* + VOODOO_INPUT_* keys
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

OSDefineMetaClassAndStructors(MavericksVoodooInput, IOService)

static uint32_t read_u32_prop(IOService *p, const char *key, uint32_t dflt) {
    OSNumber *n = OSDynamicCast(OSNumber, p->getProperty(key));
    return n ? (uint32_t)n->unsigned32BitValue() : dflt;
}

bool MavericksVoodooInput::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fProvider = provider;
    uint32_t lmx = read_u32_prop(provider, VOODOO_INPUT_LOGICAL_MAX_X_KEY, 0);
    uint32_t lmy = read_u32_prop(provider, VOODOO_INPUT_LOGICAL_MAX_Y_KEY, 0);
    /* Transport policy stays in the router (it already reads provider props): BT default, USB when the
     * satellite advertises it. The backend just receives the typed value. */
    mavericks_amd_terminal_transport_t xport = MAVERICKS_AMD_TERMINAL_XPORT_BT;
    OSString *tp = OSDynamicCast(OSString, provider->getProperty("MT2 Transport"));
    if (tp && tp->isEqualTo("USB")) xport = MAVERICKS_AMD_TERMINAL_XPORT_USB;

    fBackend = OSTypeAlloc(MavericksTerminalBackend);
    if (fBackend && !fBackend->start(this, xport, lmx, lmy)) { fBackend->release(); fBackend = 0; }
    if (!fBackend)
        IOLog("MavericksVoodooInput: WARNING terminal backend start failed; no cursor\n");

    setProperty(VOODOO_INPUT_IDENTIFIER, kOSBooleanTrue);  // satellites locate us by this
    registerService();
    MAVERICKS_DLOG(1, "MavericksVoodooInput: up (LmaxX=%u LmaxY=%u)", lmx, lmy);
    return true;
}

void MavericksVoodooInput::stop(IOService *provider) {
    fProvider = 0;   // routing fence: a late message() drops on the provider==fProvider check
    if (fBackend) { fBackend->stop(this); fBackend->release(); fBackend = 0; }
    IOService::stop(provider);
}

IOReturn MavericksVoodooInput::message(UInt32 type, IOService *provider, void *argument) {
    if (type == kIOMessageVoodooInputMessage && provider == fProvider && argument) {
        if (fBackend) fBackend->handleEvent((const VoodooInputEvent *)argument);
        return kIOReturnSuccess;
    }
    if (type == kIOMessageVoodooInputUpdateDimensionsMessage && provider == fProvider) {
        if (fBackend)
            fBackend->updateDimensions(
                read_u32_prop(fProvider, VOODOO_INPUT_LOGICAL_MAX_X_KEY, fBackend->logicalMaxX()),
                read_u32_prop(fProvider, VOODOO_INPUT_LOGICAL_MAX_Y_KEY, fBackend->logicalMaxY()));
        return kIOReturnSuccess;
    }
    return IOService::message(type, provider, argument);
}
