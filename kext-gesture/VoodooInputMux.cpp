#include "VoodooInputMux.h"
#include "MT2Gesture.h"            // com_schmonz_MT2Gesture (the engine)
#include "mt2_log.h"              // MT2_DLOG
#include "voodoo_wire.h"           // wire VoodooInputEvent + kIOMessage* + key macros
#include "mt2_voodoo_translate.h"  // mt2_frame_from_voodoo (extern "C")
#include <libkern/c++/OSNumber.h>

extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_VoodooInput, IOService)

static uint32_t read_u32_prop(IOService *p, const char *key, uint32_t dflt) {
    OSNumber *n = OSDynamicCast(OSNumber, p->getProperty(key));
    return n ? (uint32_t)n->unsigned32BitValue() : dflt;
}

bool com_schmonz_VoodooInput::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fProvider    = provider;
    fLogicalMaxX = read_u32_prop(provider, VOODOO_INPUT_LOGICAL_MAX_X_KEY, 0);
    fLogicalMaxY = read_u32_prop(provider, VOODOO_INPUT_LOGICAL_MAX_Y_KEY, 0);
    setProperty(VOODOO_INPUT_IDENTIFIER, kOSBooleanTrue);  // satellites locate us by this
    registerService();
    if (!fLogicalMaxX || !fLogicalMaxY)   // dimension-less satellite -> translator identity fallback
        IOLog("VoodooInputMux: WARNING zero logical max (X=%u Y=%u); coordinates unscaled\n",
              fLogicalMaxX, fLogicalMaxY);
    MT2_DLOG(1, "VoodooInputMux: up (LmaxX=%u LmaxY=%u)", fLogicalMaxX, fLogicalMaxY);
    return true;
}

void com_schmonz_VoodooInput::stop(IOService *provider) {
    fProvider = 0;                        // fence: a late message() drops on the provider==fProvider check
    if (gActiveMT2Gesture) gActiveMT2Gesture->quiesceDelivery();  // drain any in-flight submitFrame(this,...)
    IOService::stop(provider);
}

IOReturn com_schmonz_VoodooInput::message(UInt32 type, IOService *provider, void *argument) {
    if (type == kIOMessageVoodooInputMessage && provider == fProvider && argument) {
        const VoodooInputEvent *w = (const VoodooInputEvent *)argument;
        mt2_frame f = mt2_frame_from_voodoo(w, fLogicalMaxX, fLogicalMaxY);
        /* Feed the shared engine seam. NOTE (sub-project 1 boundary): submitFrame only DELIVERS
         * once someone has called gActiveMT2Gesture->connectionEstablished(this, mode, policy,
         * &terminalSink) — the session drops frames whose source != active_source. That terminal
         * sink (inject_encoded -> a nub the recognizer binds) is the revived synthetic consumer of
         * SUB-PROJECT 2. Until then this is a safe no-op: the translate path is exercised, frames
         * are dropped by the active-source guard, nothing crashes. */
        if (gActiveMT2Gesture) gActiveMT2Gesture->submitFrame(this, &f);
        return kIOReturnSuccess;
    }
    if (type == kIOMessageVoodooInputUpdateDimensionsMessage && provider == fProvider) {
        fLogicalMaxX = read_u32_prop(fProvider, VOODOO_INPUT_LOGICAL_MAX_X_KEY, fLogicalMaxX);
        fLogicalMaxY = read_u32_prop(fProvider, VOODOO_INPUT_LOGICAL_MAX_Y_KEY, fLogicalMaxY);
        return kIOReturnSuccess;
    }
    return IOService::message(type, provider, argument);
}
