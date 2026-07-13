#ifndef MT2GESTURE_H
#define MT2GESTURE_H
#include <IOKit/IOService.h>
#include "mt2_session.h"
#include "amd_shim.h"
#include "mt2_synth_amd.h"

/* The transport nub + session/conditioning core. It creates NO multitouch device of its own —
 * Apple's genuine driver does (BNBTrackpadDevice over BT, AppleUSBMultitouchDriver over USB). The
 * in-kernel readers (MT2BTReader, MT2USBReader) submit decoded frames through
 * connectionEstablished()/submitFrame(); the session conditions them and the ACTIVE reader's
 * registered transport sink delivers to Apple's genuine consumer. See MT2Gesture.cpp. */
class IOTimerEventSource;
class IOWorkLoop;

/* Per-transport delivery, registered by the active reader at connectionEstablished().
 *   feed_frame:       encode + deliver one session-conditioned frame to Apple's genuine consumer
 *                     (BT: mt1_encode -> handleTouchFrame; USB: mt1_encode + checksum -> handleReport).
 *   post_button_edge: forward a change in the device's REAL physical button as Apple's click mask
 *                     (0 release / 0x1 primary / 0x2 secondary) — a hardware button, not a synth click.
 *   inject_encoded:   DEBUG seam — deliver already-encoded 0x28 bytes (the user client's feedFrame);
 *                     NULL = unsupported on this transport (returns kIOReturnNotReady).
 * All calls arrive under the engine's session lock; implementations must not re-enter the engine. */
typedef struct {
    void (*feed_frame)(void *ctx, const mt2_frame *frame);
    void (*post_button_edge)(void *ctx, unsigned mask);
    IOReturn (*inject_encoded)(void *ctx, const unsigned char *bytes, unsigned int len);
    void *ctx;
} mt2_transport_sink_t;

class com_schmonz_MT2Gesture : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2Gesture)
    mt2_session_t fSession;               /* pure functional core: owns all post-decode logic */
    mt2_session_sink_t fSink;             /* effects seam handed to the session: trampolines below */
    mt2_transport_sink_t fXport;          /* the ACTIVE reader's delivery, registered at
                                             connectionEstablished, cleared at connectionClosed */
    IOWorkLoop *fPipeWL;                  /* hosts the watchdog timer */
    IOTimerEventSource *fIdleTimer;       /* the silence-watchdog timer the session arms */
    IOLock *fSessionLock;                 /* serializes all fSession + fXport access: the timer
                                             fires on fPipeWL while submitFrame runs on the
                                             caller's transport workloop */

    /* Session-sink trampolines (ctx = this): dispatch each session effect to fXport.
       NULL-guarded — after connectionClosed() a late watchdog fire must be a no-op. */
    static void sink_post_button_edge(void *ctx, unsigned mask);
    static void sink_feed_frame(void *ctx, const mt2_frame *frame);
    static void sink_arm_timer(void *ctx, uint32_t ms);
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    /* Session-backed transport path: a reader arms a connection (registering its policy row +
     * delivery sink), then submits decoded mt2_frame frames; the session decides what
     * reaches the device via the registered sink. connectionClosed() deregisters: after it
     * returns, no sink callback of that reader's will run (clear-under-lock + lifecycle reset). */
    void connectionEstablished(IOService *source, mt2_transport_mode_t mode,
                               const mt2_session_policy_t *policy,
                               const mt2_transport_sink_t *sink);
    void connectionClosed(IOService *source);

    /* Barrier for teardown paths that clear delivery targets OUTSIDE the engine (e.g. the BT
     * control reader nulling gGenuineBnb before tearing the genuine BNB down): acquire+release
     * the session lock, so any in-flight sink delivery completes and the caller's prior stores
     * are visible to later sink calls before the caller proceeds to destroy the target. */
    void quiesceDelivery(void);
    void submitFrame(IOService *source, const mt2_frame *tf);
    static void idleTimeout(OSObject *owner, IOTimerEventSource *sender);

    /* DEBUG/TEST seam: the user client routes injected encoded 0x28 bytes here, through the
     * active transport's inject_encoded (bypasses the session). NotReady if none registered. */
    IOReturn feedFrame(const unsigned char *bytes, unsigned int len);

    /* Synthetic terminal: build (ref-counted) the fabricated AMD + HIDShell under this nub,
     * register a kSynthSink that encodes frames via mt1_encode and delivers them to the AMD's
     * handleTouchFrame. beginSyntheticTerminal calls connectionEstablished (which resets the
     * session); endSyntheticTerminal calls connectionClosed then tears the AMD down outside the
     * lock when the last consumer releases. Both the mux (Task 7) and the UserClient (Task 5)
     * will drive this. */
    IOReturn beginSyntheticTerminal(IOService *source, mt2_transport_mode_t mode,
                                    const mt2_session_policy_t *policy);
    void     endSyntheticTerminal(IOService *source);
    AppleMultitouchDevice *synthAMD() const { return mt2_synth_amd_amd(fSynthCtx); }   /* for the sink glue */

private:
    mt2_synth_amd_ctx *fSynthCtx;
    int fSynthRefs;
};
#endif
