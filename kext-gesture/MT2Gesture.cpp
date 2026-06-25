/*
 * MT2Gesture — the transport nub + session/conditioning core for the MT2 stack.
 *
 * It does NOT create a multitouch device of its own. Apple's genuine driver does that: over BT a
 * manually-started BNBTrackpadDevice spawns its own AppleMultitouchDevice (AMD); over USB a
 * manually-started AppleUSBMultitouchDriver owns the interface. This nub:
 *   - hosts the shared mt2_session (settle gate, MakeTouch/Touching/BreakTouch lifecycle, liftoff)
 *     that the in-kernel readers feed via connectionEstablished()/submitFrame(); and
 *   - owns the click sink, which feeds the conditioned stream + device-button edges to that genuine
 *     AMD through fBnbTarget (set by the BT interpose; see MT2BTReader.cpp setBnbTarget).
 * It also advertises a debug/test user client (inject pre-encoded 0x28 frames) and the
 * debug.mt2_log sysctl.
 *
 * (The retired synthetic approach — a fabricated AppleMultitouchDevice + an MT2HIDShell we drove
 * ourselves — is documented in docs/mt-stack/explanation.md → "Retired synthetic approach".)
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <kern/clock.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "amd_shim.h"          /* AppleMultitouchDevice handleTouchFrame / handlePointerEventFromDevice */
#include "MT2Gesture.h"
#include "mt1_encode.h"
#include "mt2_log.h"           /* gMT2LogLevel, MT2_DLOG, sysctl register/unregister */

OSDefineMetaClassAndStructors(com_schmonz_MT2Gesture, IOService)

/* Kernel uptime in milliseconds — the clock the session reads through the shell. */
static uint32_t uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* Sink: post a device-button edge (mask 0/0x1/0x2) to the genuine AMD's native button path — the
 * physical click + two-finger right-click edges the recognizer's tap path can't produce on its own.
 * fBnbTarget is the genuine spawned AppleMultitouchDevice (set by the BT interpose); NULL-guard so an
 * edge arriving before bring-up is dropped rather than crashing. */
void com_schmonz_MT2Gesture::sink_post_click(void *ctx, unsigned mask) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (!self->fBnbTarget) return;
    MT2_DLOG(2, "post_click mask=0x%x -> bnbAMD", mask);
    ((AppleMultitouchDevice *)self->fBnbTarget)->handlePointerEventFromDevice(0, 0, mask, 0);
}
/* Sink: MT1-encode the session-conditioned touch frame (lifecycle states + liftoff) and feed it to
 * the genuine spawned AppleMultitouchDevice (fBnbTarget), which Apple's recognizer turns into cursor
 * + gestures. NULL-guard for frames arriving before the BT interpose sets the target. */
void com_schmonz_MT2Gesture::sink_feed_frame(void *ctx, const touch_frame_t *frame) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (!self->fBnbTarget) return;
    /* EDGE-CLAMP PROBE (debug.mt2_log>=2): per-frame decoded contact-0 x/y at the encode point, to
     * correlate the faithful decoded position against the recognizer's normalization. */
    if (frame->ntouches > 0)
        MT2_DLOG(2, "feed x=%d y=%d -> bnbAMD", frame->touches[0].x, frame->touches[0].y);
    uint8_t mt1[256];
    int n = mt1_encode(frame, mt1, sizeof(mt1), uptime_ms());
    if (n <= 0) return;
    ((AppleMultitouchDevice *)self->fBnbTarget)->handleTouchFrame(mt1, (unsigned int)n);
}
/* Sink: (re)arm the silence-watchdog timer. */
void com_schmonz_MT2Gesture::sink_arm_timer(void *ctx, uint32_t ms) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (self->fIdleTimer) self->fIdleTimer->setTimeoutMS(ms);
}

/* A reader (BT/USB) announces its transport; the session resets and arms settle. */
void com_schmonz_MT2Gesture::connectionEstablished(IOService *source,
                                                   mt2_transport_mode_t mode) {
    if (fSessionLock) IOLockLock(fSessionLock);
    if (fIdleTimer) fIdleTimer->cancelTimeout();
    mt2_session_connect(&fSession, (uintptr_t)source, mode, uptime_ms());
    if (fSessionLock) IOLockUnlock(fSessionLock);
    IOLog("MT2Gesture: connection established (src=%p mode=%d)\n", source, (int)mode);
}
/* A reader submits one decoded frame; the session decides what reaches the device. */
void com_schmonz_MT2Gesture::submitFrame(IOService *source, const touch_frame_t *tf) {
    if (fSessionLock) IOLockLock(fSessionLock);
    mt2_session_frame(&fSession, (uintptr_t)source, tf, uptime_ms(), &fSink);
    if (fSessionLock) IOLockUnlock(fSessionLock);
}

/* DEBUG/TEST seam (user client selector 0): inject an already-encoded MT1 0x28 report straight to
 * the genuine AMD (fBnbTarget), same as sink_feed_frame's final step. Bypasses the session so a test
 * tool can run the session/encode in userspace and feed the exact bytes. Returns the device's status
 * (e.g. ~0xE00002BC if not yet ready) — benign, never panics. */
IOReturn com_schmonz_MT2Gesture::feedFrame(const unsigned char *bytes, unsigned int len) {
    if (!fBnbTarget) return kIOReturnNotReady;
    return ((AppleMultitouchDevice *)fBnbTarget)->handleTouchFrame((unsigned char *)bytes, len);
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
    fBnbTarget = 0;
    gActiveMT2Gesture = this;   /* let the in-kernel readers feed us */
    mt2_log_register();         /* debug.mt2_log sysctl (single-instance nub owns its lifetime) */

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
    mt2_lifecycle_reset(&fSession.lifecycle);
    fSink.post_click = &com_schmonz_MT2Gesture::sink_post_click;
    fSink.feed_frame = &com_schmonz_MT2Gesture::sink_feed_frame;
    fSink.arm_timer  = &com_schmonz_MT2Gesture::sink_arm_timer;
    fSink.ctx = this;
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

    /* The genuine BNB (BT) / genuine AppleUSBMultitouchDriver (USB) spawn and drive the real
     * AppleMultitouchDevice that is the input source and the prefpane target. This nub creates no
     * device of its own; the session sink feeds that genuine AMD via fBnbTarget. */
    IOLog("MT2Gesture: nub up — genuine AMD drives input + owns the pane\n");
    return true;
}

void com_schmonz_MT2Gesture::stop(IOService *provider) {
    /* Tear the watchdog timer down FIRST so a late fire can't drive the sink during teardown. */
    if (fIdleTimer) {
        fIdleTimer->cancelTimeout();
        if (fPipeWL) fPipeWL->removeEventSource(fIdleTimer);
        fIdleTimer->release(); fIdleTimer = 0;
    }
    if (fPipeWL) { fPipeWL->release(); fPipeWL = 0; }
    /* Timer is fully removed above (no more idleTimeout), so the lock has no more
     * users and is safe to free. */
    if (fSessionLock) { IOLockFree(fSessionLock); fSessionLock = 0; }
    if (gActiveMT2Gesture == this) gActiveMT2Gesture = 0;
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
