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
    if (gActiveMT2Gesture)
        gActiveMT2Gesture->beginSyntheticTerminal(this, MT2_EVENT_DRIVEN, &mt2_policy_default);
    return true;
}

void com_schmonz_VoodooInput::stop(IOService *provider) {
    if (gActiveMT2Gesture) gActiveMT2Gesture->endSyntheticTerminal(this);
    fProvider = 0;                        // fence: a late message() drops on the provider==fProvider check
    if (gActiveMT2Gesture) gActiveMT2Gesture->quiesceDelivery();  // drain any in-flight submitFrame(this,...)
    IOService::stop(provider);
}

IOReturn com_schmonz_VoodooInput::message(UInt32 type, IOService *provider, void *argument) {
    if (type == kIOMessageVoodooInputMessage && provider == fProvider && argument) {
        const VoodooInputEvent *w = (const VoodooInputEvent *)argument;
        mt2_frame f = mt2_frame_from_voodoo(w, fLogicalMaxX, fLogicalMaxY);
        /* Deliver to the shared engine. As of sub-project 2, start() has called
         * beginSyntheticTerminal(this,...), so `this` is the active source with kSynthSink
         * registered — the frame flows through the session to the fabricated AMD (cursor). */
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
