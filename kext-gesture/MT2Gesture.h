#ifndef MT2GESTURE_H
#define MT2GESTURE_H
#include <IOKit/IOService.h>
#include "amd_shim.h"
#include "mt2_session.h"

/* The transport nub + session/conditioning core. It creates NO multitouch device of its own —
 * Apple's genuine driver does (BNBTrackpadDevice over BT, AppleUSBMultitouchDriver over USB). The
 * in-kernel readers (MT2BTReader, MT2USBReader) submit decoded frames through
 * connectionEstablished()/submitFrame(); the session conditions them and the sink feeds the genuine
 * AMD via fBnbTarget. See MT2Gesture.cpp. */
class IOTimerEventSource;
class IOWorkLoop;

class com_schmonz_MT2Gesture : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2Gesture)
    void *fBnbTarget;                     /* the genuine spawned AppleMultitouchDevice (BNB+0x1b0 over
                                             BT). The session sink feeds THIS the conditioned
                                             (lifecycle/liftoff) stream + device-button edges. */
    mt2_session_t fSession;               /* pure functional core: owns all post-decode
                                             logic (settle/guard/lift-drop/decel/click) */
    mt2_session_sink_t fSink;             /* effects seam: callbacks drive IOKit */
    IOWorkLoop *fPipeWL;                  /* hosts the decel timer */
    IOTimerEventSource *fIdleTimer;       /* the idle/decel timer the session arms */
    IOLock *fSessionLock;                 /* serializes all fSession access: the timer
                                             fires on fPipeWL while submitFrame runs on
                                             the caller's transport workloop */

    /* Sink callbacks (ctx = this): translate session effects into IOKit calls. */
    static void sink_post_click(void *ctx, unsigned mask);
    static void sink_feed_frame(void *ctx, const touch_frame_t *frame);
    static void sink_arm_timer(void *ctx, uint32_t ms);
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /* Point the session sink at the genuine spawned AMD (or NULL to detach before it is freed). */
    void setBnbTarget(void *amd) { fBnbTarget = amd; }

    /* Session-backed transport path: a reader arms a connection, then submits decoded
     * touch_frame_t frames; the session decides what reaches the device via the sink. */
    void connectionEstablished(IOService *source, mt2_transport_mode_t mode);
    void submitFrame(IOService *source, const touch_frame_t *tf);
    static void idleTimeout(OSObject *owner, IOTimerEventSource *sender);

    /* DEBUG/TEST seam: the user client routes injected encoded 0x28 bytes here, straight
     * to the genuine AMD (bypasses the session) for hands-free on-device testing. */
    IOReturn feedFrame(const unsigned char *bytes, unsigned int len);
};
#endif
