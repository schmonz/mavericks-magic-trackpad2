#include "MavericksTerminalBackend.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <libkern/c++/OSNumber.h>
#include "mavericks_voodoo_translate.h"  // mavericks_frame_from_voodoo (extern "C")
#include "mavericks_log.h"               // MAVERICKS_DLOG + gMavericksBattOverride

OSDefineMetaClassAndStructors(MavericksTerminalBackend, OSObject)

static uint32_t uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* Sink trampolines: dispatch each session effect to the backend's own fabricated AMD. */
static void be_feed_frame(void *ctx, const MavericksTouchFrame *frame) {
    MavericksTerminalBackend *self = (MavericksTerminalBackend *)ctx;
    mavericks_amd_terminal_feed(self->synthCtx(), frame, (uint32_t)uptime_ms());
}
static void be_post_button_edge(void *ctx, unsigned mask) {
    MavericksTerminalBackend *self = (MavericksTerminalBackend *)ctx;
    mavericks_amd_terminal_button(self->synthCtx(), mask);
}
static void be_arm_timer(void *ctx, uint32_t ms) { ((MavericksTerminalBackend *)ctx)->armIdle(ms); }

void MavericksTerminalBackend::armIdle(uint32_t ms) { if (fIdle) fIdle->setTimeoutMS(ms); }

void MavericksTerminalBackend::idleTick(OSObject *owner, IOTimerEventSource *) {
    MavericksTerminalBackend *self = OSDynamicCast(MavericksTerminalBackend, owner);
    if (!self) return;
    mavericks_session_sink_t sink = { be_post_button_edge, be_feed_frame, be_arm_timer, self };
    if (self->fLock) IOLockLock(self->fLock);
    mavericks_session_timer(&self->fSession, &sink);
    if (self->fLock) IOLockUnlock(self->fLock);
}

bool MavericksTerminalBackend::start(IOService *nub, mavericks_amd_terminal_transport_t transport,
                                     uint32_t logicalMaxX, uint32_t logicalMaxY) {
    fLogicalMaxX = logicalMaxX;
    fLogicalMaxY = logicalMaxY;
    if (!fLogicalMaxX || !fLogicalMaxY)
        IOLog("MavericksTerminalBackend: WARNING zero logical max (X=%u Y=%u); coordinates unscaled\n",
              fLogicalMaxX, fLogicalMaxY);
    fSynth = 0; fWL = 0; fIdle = 0; fLock = IOLockAlloc(); fLastBattPct = -1;
    fSynth = mavericks_amd_terminal_build(nub, transport);   /* fail-soft */
    if (!fSynth) IOLog("MavericksTerminalBackend: WARNING synthetic AMD build failed; no cursor\n");
    fWL = IOWorkLoop::workLoop();
    fIdle = fWL ? IOTimerEventSource::timerEventSource(this, &idleTick) : 0;
    if (fIdle) fWL->addEventSource(fIdle);
    mavericks_session_connect(&fSession, (uintptr_t)this, MAVERICKS_EVENT_DRIVEN,
                              &mavericks_policy_default, (uint32_t)uptime_ms());
    return true;
}

void MavericksTerminalBackend::handleEvent(const VoodooInputEvent *ev) {
    MavericksTouchFrame f = mavericks_frame_from_voodoo(ev, fLogicalMaxX, fLogicalMaxY);
    mavericks_session_sink_t sink = { be_post_button_edge, be_feed_frame, be_arm_timer, this };
    if (fLock) IOLockLock(fLock);
    mavericks_session_frame(&fSession, (uintptr_t)this, &f, (uint32_t)uptime_ms(), &sink);
    if (fLock) IOLockUnlock(fLock);
}

void MavericksTerminalBackend::updateDimensions(uint32_t logicalMaxX, uint32_t logicalMaxY) {
    fLogicalMaxX = logicalMaxX;
    fLogicalMaxY = logicalMaxY;
}

void MavericksTerminalBackend::stop(IOService *nub) {
    /* Barrier: drain any handleEvent holding fLock before we tear down fSynth / free fLock (UAF-safe,
     * matches the old mux stop()). The mux clears its provider fence BEFORE calling this, so no new
     * handleEvent enters. */
    if (fLock) { IOLockLock(fLock); IOLockUnlock(fLock); }
    if (fIdle) { fIdle->cancelTimeout(); if (fWL) fWL->removeEventSource(fIdle); fIdle->release(); fIdle = 0; }
    if (fWL) { fWL->release(); fWL = 0; }
    mavericks_amd_terminal_teardown(nub, fSynth); fSynth = 0;
    if (fLock) { IOLockFree(fLock); fLock = 0; }
}

void MavericksTerminalBackend::publishBattery(uint8_t pct) {
    /* debug.mt2_batt override: force a value for prefpane UI testing (-1 = off / use the real value). */
    if (gMavericksBattOverride >= 0 && gMavericksBattOverride <= 100) pct = (uint8_t)gMavericksBattOverride;
    if (pct > 100) return;                             /* capacity is 0-100; ignore out-of-range */
    if ((int)pct == fLastBattPct) return;              /* dedup: same value on our one AMD node */
    AppleMultitouchDevice *amd = mavericks_amd_terminal_amd(fSynth);   /* NULL until built / during teardown */
    if (!amd) return;                                  /* self-fence: no node yet */
    fLastBattPct = (int)pct;
    OSNumber *n = OSNumber::withNumber((unsigned long long)pct, 32);
    if (n) { ((IOService *)amd)->setProperty("BatteryPercent", n); n->release(); }
    MAVERICKS_DLOG(1, "battery = %u%% -> published BatteryPercent", (unsigned)pct);
}
