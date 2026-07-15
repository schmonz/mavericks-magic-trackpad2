#include "VoodooInputMux.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include "mt2_log.h"              // MT2_DLOG
#include "voodoo_wire.h"           // wire VoodooInputEvent + kIOMessage* + key macros
#include "mt2_voodoo_translate.h"  // mt2_frame_from_voodoo (extern "C")
#include <libkern/c++/OSNumber.h>

OSDefineMetaClassAndStructors(com_schmonz_VoodooInput, IOService)

static uint32_t uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

static uint32_t read_u32_prop(IOService *p, const char *key, uint32_t dflt) {
    OSNumber *n = OSDynamicCast(OSNumber, p->getProperty(key));
    return n ? (uint32_t)n->unsigned32BitValue() : dflt;
}

/* Sink trampolines: dispatch each session effect to the mux's own synthetic AMD. */
static void mux_feed_frame(void *ctx, const mt2_frame *frame) {
    com_schmonz_VoodooInput *self = (com_schmonz_VoodooInput *)ctx;
    mt2_synth_amd_feed(self->synthCtx(), frame, (uint32_t)uptime_ms());
}
static void mux_post_button_edge(void *ctx, unsigned mask) { (void)ctx; (void)mask; }
static void mux_arm_timer(void *ctx, uint32_t ms) { ((com_schmonz_VoodooInput *)ctx)->armIdle(ms); }

void com_schmonz_VoodooInput::armIdle(uint32_t ms) { if (fIdle) fIdle->setTimeoutMS(ms); }

void com_schmonz_VoodooInput::idleTick(OSObject *owner, IOTimerEventSource *) {
    com_schmonz_VoodooInput *self = OSDynamicCast(com_schmonz_VoodooInput, owner);
    if (!self) return;
    mt2_session_sink_t sink = { mux_post_button_edge, mux_feed_frame, mux_arm_timer, self };
    if (self->fLock) IOLockLock(self->fLock);
    mt2_session_timer(&self->fSession, &sink);
    if (self->fLock) IOLockUnlock(self->fLock);
}

bool com_schmonz_VoodooInput::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fProvider    = provider;
    fLogicalMaxX = read_u32_prop(provider, VOODOO_INPUT_LOGICAL_MAX_X_KEY, 0);
    fLogicalMaxY = read_u32_prop(provider, VOODOO_INPUT_LOGICAL_MAX_Y_KEY, 0);
    if (!fLogicalMaxX || !fLogicalMaxY)   // dimension-less satellite -> translator identity fallback
        IOLog("VoodooInputMux: WARNING zero logical max (X=%u Y=%u); coordinates unscaled\n",
              fLogicalMaxX, fLogicalMaxY);

    fSynth = 0; fWL = 0; fIdle = 0; fLock = IOLockAlloc();
    fSynth = mt2_synth_amd_build(this);   /* under the mux nub itself; fail-soft */
    if (!fSynth) IOLog("VoodooInputMux: WARNING synthetic AMD build failed; no cursor\n");
    fWL = IOWorkLoop::workLoop();
    fIdle = fWL ? IOTimerEventSource::timerEventSource(this, &idleTick) : 0;
    if (fIdle) fWL->addEventSource(fIdle);
    mt2_session_connect(&fSession, (uintptr_t)this, MT2_EVENT_DRIVEN, &mt2_policy_default, (uint32_t)uptime_ms());

    setProperty(VOODOO_INPUT_IDENTIFIER, kOSBooleanTrue);  // satellites locate us by this
    registerService();
    MT2_DLOG(1, "VoodooInputMux: up (LmaxX=%u LmaxY=%u)", fLogicalMaxX, fLogicalMaxY);
    return true;
}

void com_schmonz_VoodooInput::stop(IOService *provider) {
    fProvider = 0;                        // fence: a late message() drops on the provider==fProvider check
    /* Quiesce: drain any message() that passed the fence and holds fLock inside mt2_session_frame,
     * before we tear down fSynth / free fLock. With fProvider==0 no new message() enters the fLock
     * region, so this barrier makes the teardown UAF-safe (matches the old quiesceDelivery pattern).
     * Needed for the edge case where the mux is torn down while a live satellite is still sending. */
    if (fLock) { IOLockLock(fLock); IOLockUnlock(fLock); }
    if (fIdle) { fIdle->cancelTimeout(); if (fWL) fWL->removeEventSource(fIdle); fIdle->release(); fIdle = 0; }
    if (fWL) { fWL->release(); fWL = 0; }
    mt2_synth_amd_teardown(this, fSynth); fSynth = 0;
    if (fLock) { IOLockFree(fLock); fLock = 0; }
    IOService::stop(provider);
}

IOReturn com_schmonz_VoodooInput::message(UInt32 type, IOService *provider, void *argument) {
    if (type == kIOMessageVoodooInputMessage && provider == fProvider && argument) {
        const VoodooInputEvent *w = (const VoodooInputEvent *)argument;
        mt2_frame f = mt2_frame_from_voodoo(w, fLogicalMaxX, fLogicalMaxY);
        mt2_session_sink_t sink = { mux_post_button_edge, mux_feed_frame, mux_arm_timer, this };
        if (fLock) IOLockLock(fLock);
        mt2_session_frame(&fSession, (uintptr_t)this, &f, (uint32_t)uptime_ms(), &sink);
        if (fLock) IOLockUnlock(fLock);
        return kIOReturnSuccess;
    }
    if (type == kIOMessageVoodooInputUpdateDimensionsMessage && provider == fProvider) {
        fLogicalMaxX = read_u32_prop(fProvider, VOODOO_INPUT_LOGICAL_MAX_X_KEY, fLogicalMaxX);
        fLogicalMaxY = read_u32_prop(fProvider, VOODOO_INPUT_LOGICAL_MAX_Y_KEY, fLogicalMaxY);
        return kIOReturnSuccess;
    }
    return IOService::message(type, provider, argument);
}
