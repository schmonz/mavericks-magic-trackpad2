/*
 * MT2Gesture — the transport nub + session/conditioning core for the MT2 stack.
 *
 * For the MT2 itself it does NOT create a multitouch device of its own — Apple's genuine driver
 * does that: over BT a manually-started BNBTrackpadDevice spawns its own AppleMultitouchDevice
 * (AMD); over USB a manually-started AppleUSBMultitouchDriver owns the interface. This nub:
 *   - hosts the shared mt2_session (settle gate, MakeTouch/Touching/BreakTouch lifecycle, liftoff)
 *     that the in-kernel readers feed via connectionEstablished()/submitFrame(); and
 *   - dispatches the session's effects to the active reader's registered transport sink
 *     (registered at connectionEstablished, deregistered at connectionClosed) — encode and
 *     delivery are the reader's business; this shell owns only session + lock + timer.
 * It also advertises a debug/test user client (inject pre-encoded 0x28 frames) and the
 * debug.mt2_log sysctl.
 *
 * The synthetic terminal (beginSyntheticTerminal / kSynthSink) was removed in Task 3: it now
 * lives per-mux in VoodooInputMux, where each satellite owns its own fabricated AMD + HIDShell.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <kern/clock.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "MT2Gesture.h"
#include "mt2_log.h"           /* gMT2LogLevel, MT2_DLOG, sysctl register/unregister */
#include "mt2_amd_probe.h"    /* debug.mt2_amd_probe oracle (build/teardown/churn + live-AMD count) */

OSDefineMetaClassAndStructors(com_schmonz_MT2Gesture, IOService)

/* Kernel uptime in milliseconds — the clock the session reads through the shell. */
static uint32_t uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* Sink trampolines: dispatch each session effect to the ACTIVE reader's registered transport
 * sink (fXport). NULL-guarded so a watchdog fire after connectionClosed() is a no-op. Encode
 * and delivery are the READER's business now — this shell owns only session + lock + timer. */
void com_schmonz_MT2Gesture::sink_post_button_edge(void *ctx, unsigned mask) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (self->fXport.post_button_edge) self->fXport.post_button_edge(self->fXport.ctx, mask);
}
void com_schmonz_MT2Gesture::sink_feed_frame(void *ctx, const MavericksTouchFrame *frame) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (self->fXport.feed_frame) self->fXport.feed_frame(self->fXport.ctx, frame);
}
/* Sink: (re)arm the silence-watchdog timer. */
void com_schmonz_MT2Gesture::sink_arm_timer(void *ctx, uint32_t ms) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (self->fIdleTimer) self->fIdleTimer->setTimeoutMS(ms);
}

/* A reader announces its transport: register its policy row + delivery sink, reset the session. */
void com_schmonz_MT2Gesture::connectionEstablished(IOService *source, mt2_transport_mode_t mode,
                                                   const mt2_session_policy_t *policy,
                                                   const mt2_transport_sink_t *sink) {
    if (fSessionLock) IOLockLock(fSessionLock);
    if (fIdleTimer) fIdleTimer->cancelTimeout();
    fXport = *sink;
    mt2_session_connect(&fSession, (uintptr_t)source, mode, policy, uptime_ms());
    if (fSessionLock) IOLockUnlock(fSessionLock);
    IOLog("MT2Gesture: connection established (src=%p mode=%d)\n", source, (int)mode);
}

/* Deregister: after this returns no sink callback of this reader's runs. Under the same lock
 * submitFrame and the timer serialize on; a racing frame either completes first or is dropped
 * by the single-active guard; the lifecycle reset makes a late timer flush produce nothing. */
void com_schmonz_MT2Gesture::connectionClosed(IOService *source) {
    if (fSessionLock) IOLockLock(fSessionLock);
    if ((uintptr_t)source == fSession.active_source) {
        if (fIdleTimer) fIdleTimer->cancelTimeout();
        fXport.feed_frame = 0; fXport.post_button_edge = 0; fXport.inject_encoded = 0; fXport.ctx = 0;
        fSession.active_source = 0;
        mt2_lifecycle_reset(&fSession.lifecycle);
        IOLog("MT2Gesture: connection closed (src=%p)\n", source);
    }
    if (fSessionLock) IOLockUnlock(fSessionLock);
}

/* See header. Deliberately does NOT clear fXport or the session: the caller is not the
 * registered source (that's connectionClosed's job) — it only needs in-flight delivery drained
 * + a memory barrier for its target-clearing stores. */
void com_schmonz_MT2Gesture::quiesceDelivery(void) {
    if (!fSessionLock) return;
    IOLockLock(fSessionLock);
    IOLockUnlock(fSessionLock);
}

/* A reader submits one decoded frame; the session decides what reaches the device. */
void com_schmonz_MT2Gesture::submitFrame(IOService *source, const MavericksTouchFrame *tf) {
    if (fSessionLock) IOLockLock(fSessionLock);
    mt2_session_frame(&fSession, (uintptr_t)source, tf, uptime_ms(), &fSink);
    if (fSessionLock) IOLockUnlock(fSessionLock);
}

/* DEBUG/TEST seam (user client selector 0): route already-encoded 0x28 bytes through the active
 * transport's inject_encoded (BT: straight to the genuine AMD's handleTouchFrame, as before).
 * NotReady when no transport (or one without inject support) is registered. Improvement over the
 * old fBnbTarget guard: inject now works as soon as the AMD spawns, not only after the first touch
 * frame cached it — hands-free testing needs no priming touch. */
IOReturn com_schmonz_MT2Gesture::feedFrame(const unsigned char *bytes, unsigned int len) {
    IOReturn rc = kIOReturnNotReady;
    if (fSessionLock) IOLockLock(fSessionLock);
    if (fXport.inject_encoded) rc = fXport.inject_encoded(fXport.ctx, bytes, len);
    if (fSessionLock) IOLockUnlock(fSessionLock);
    return rc;
}
/* The silence-watchdog timer fired; let the session flush any outstanding BreakTouch. */
void com_schmonz_MT2Gesture::idleTimeout(OSObject *owner, IOTimerEventSource * /*s*/) {
    com_schmonz_MT2Gesture *self = OSDynamicCast(com_schmonz_MT2Gesture, owner);
    if (!self) return;
    if (self->fSessionLock) IOLockLock(self->fSessionLock);
    mt2_session_timer(&self->fSession, &self->fSink);
    if (self->fSessionLock) IOLockUnlock(self->fSessionLock);
}

/* The active gesture nub, published for the in-kernel readers (MT2BTReader and
 * MT2USBReader) to feed via submitFrame() — same kext, so no user client / IPC.
 * Single instance. */
com_schmonz_MT2Gesture *gActiveMT2Gesture = 0;

bool com_schmonz_MT2Gesture::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    mt2_log_register();         /* debug.mt2_log sysctl (single-instance nub owns its lifetime) */
    mt2_amd_probe_register();  /* debug.mt2_amd_probe oracle */

    /* DEBUG/TEST seam: advertise a user client so userspace tools can inject encoded
     * 0x28 frames (selector 0 -> feedFrame -> handleTouchFrame) for hands-free on-device
     * testing without a physical trackpad (tools/synth_tap, synth_feed). Read path only;
     * the in-kernel readers remain the production input. */
    setProperty("IOUserClientClass", "com_schmonz_MT2GestureUserClient");

    /* Functional-core init + the sink that drives IOKit, plus the silence-watchdog
     * timer the session arms. The session owns all post-decode logic; this shell only
     * supplies the clock, the source token, and these effect callbacks. */
    fSession.active_source = 0;
    fSession.mode = MT2_EVENT_DRIVEN;
    fSession.settle_until_ms = 0;
    fSession.last_button = 0;
    fSession.policy = mt2_policy_default;   /* inert default — active_source==0 drops all frames until
                                          a reader's connectionEstablished installs its real row;
                                          the default row keeps the struct initialized. */
    mt2_lifecycle_reset(&fSession.lifecycle);
    fSink.post_button_edge = &com_schmonz_MT2Gesture::sink_post_button_edge;
    fSink.feed_frame = &com_schmonz_MT2Gesture::sink_feed_frame;
    fSink.arm_timer  = &com_schmonz_MT2Gesture::sink_arm_timer;
    fSink.ctx = this;
    fXport.feed_frame = 0; fXport.post_button_edge = 0; fXport.inject_encoded = 0; fXport.ctx = 0;
    fSessionLock = IOLockAlloc();   /* serializes timer vs submitFrame fSession access */
    fPipeWL = IOWorkLoop::workLoop();
    fIdleTimer = 0;
    if (fPipeWL) {
        fIdleTimer = IOTimerEventSource::timerEventSource(
            this, &com_schmonz_MT2Gesture::idleTimeout);
        if (fIdleTimer && fPipeWL->addEventSource(fIdleTimer) != kIOReturnSuccess) {
            fIdleTimer->release(); fIdleTimer = 0;
        }
    }

    /* Publish ourselves so the in-kernel readers' providers resolve and IOKit
     * finishes matching our subtree. */
    registerService();

    /* For the MT2: the genuine BNB (BT) / genuine AppleUSBMultitouchDriver (USB) spawn and drive the
     * real AppleMultitouchDevice that is the input source and the prefpane target; this nub creates
     * no device for that path — the session's effects reach it through the active reader's registered
     * transport sink. VoodooInput satellites own their own fabricated AMD per-mux (VoodooInputMux). */

    /* Publish LAST: readers may call connectionEstablished the moment this is visible, so every
     * engine invariant (session fields, lock, timer, sink slots) must already hold. */
    gActiveMT2Gesture = this;

    IOLog("MT2Gesture: nub up — genuine AMD drives input + owns the pane\n");
    return true;
}

void com_schmonz_MT2Gesture::stop(IOService *provider) {
    /* Unpublish FIRST (mirror of start()'s publish-last), then drain: a reader that loaded the
     * pointer before the clear is either inside a locked engine call (drained here) or will
     * NULL-check and skip. Only then is it safe to tear the lock and timer down. */
    if (gActiveMT2Gesture == this) gActiveMT2Gesture = 0;
    quiesceDelivery();
    /* Tear the watchdog timer down next so a late fire can't drive the sink during teardown. */
    if (fIdleTimer) {
        fIdleTimer->cancelTimeout();
        if (fPipeWL) fPipeWL->removeEventSource(fIdleTimer);
        fIdleTimer->release(); fIdleTimer = 0;
    }
    if (fPipeWL) { fPipeWL->release(); fPipeWL = 0; }
    /* Timer is fully removed above (no more idleTimeout), so the lock has no more
     * users and is safe to free. */
    if (fSessionLock) { IOLockFree(fSessionLock); fSessionLock = 0; }
    mt2_amd_probe_unregister(); /* remove debug.mt2_amd_probe before the kext can unload */
    mt2_log_unregister();   /* remove debug.mt2_log before the kext can unload */
    IOService::stop(provider);
}

extern "C" {
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.schmonz.MT2Gesture, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
