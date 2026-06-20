/* MT2BTReader — in-kernel Bluetooth transport for the Magic Trackpad 2.
 *
 * A thin decoder. The MT2's multitouch frames are unreachable over BT from userspace
 * (the BT HID descriptor is boot-mouse-only; see the BT findings doc). We bind the
 * L2CAP channel directly (proven feasible by the bnbinject experiment), enable
 * multitouch with the 0xF1 SET_REPORT (the MT2's command; Apple's stock
 * BNBTrackpadDevice sends the MT1 0xD7 and so never completes), decode each frame
 * (mt2_bt_decode), and push the touch_frame_t to the MT2Gesture nub via
 * submitFrame(MT2_EVENT_DRIVEN). The shared mt2_session owns the settle gate,
 * lift-drop, deceleration/clean-lift, and click — there is no decision logic here.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include "bt_l2cap_shim.h"
#include "MT2BTReader.h"
#include "MT2Gesture.h"
#include "../src/mt2_to_mt1.h"

/* Compiled as C++ under the kext toolchain (so is mt2_bt_decode.c), so these resolve
 * with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt2_pipeline.h"   /* MT2_EVENT_DRIVEN */

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

/* PATH A (branch genuine-bnbtrackpad-citizen, design 2026-06-20-bnb-pathA-interpose-seam).
 * When true: manual-start a genuine BNBTrackpadDevice on the control channel, YIELD the
 * interrupt-channel data delegate so BNB's own listenAt succeeds (removing the §S2.5 panic
 * vector), then interpose our MT2->MT1 shim on the channel's delegate-callback slot. Default
 * false = today's behaviour (we own the delegate + feed our AppleMultitouchDevice). */
static const bool kGenuinePathA = false;

/* RE'd field offsets (findings §S2.6, 10.9 binaries; verify via re/ if a point release differs). */
#define L2CAP_DELEGATE_CB_OFF       0x110   /* IOBluetoothL2CAPChannel: delegate callback fn-ptr */
#define BNB_INTERRUPT_CHANNEL_OFF   0xf0    /* BNBDevice::_interruptChannel */

/* The genuine BNBTrackpadDevice the control reader manual-starts (Path A). The interpose
 * installer reads it (via +0xf0) to find BNB's interrupt channel, where it pokes our shim onto
 * the delegate-callback slot. Single device, single instance — a global mirrors the pattern of
 * gActiveMT2Gesture. NULL when no genuine device is up. */
IOService *gGenuineBnb = 0;

/* Saved BNB delegate the shim forwards to (single genuine device, like gGenuineBnb). */
static void *gOrigCb = 0;
static IOService *gOrigTarget = 0;
static IOBluetoothL2CAPChannel *gInterposedChannel = 0;

static uint32_t bt_uptime_ms(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint32_t)(s * 1000 + u / 1000);
}

/* IOBluetoothL2CAPChannel delegate callback: translate the raw MT2 0x31 report to MT1 and
 * hand it to BNB's original callback, so Apple's genuine parse path consumes MT1 it
 * understands. Runs in the channel's BT workloop (newDataIn context), same as Apple's own. */
static void bt_interpose_shim(IOService *target, IOBluetoothL2CAPChannel *channel,
                              unsigned short length, void *data) {
    (void)target;
    uint8_t mt1[256];
    int n = mt2_to_mt1((const uint8_t *)data, length, mt1, sizeof(mt1), bt_uptime_ms());
    if (n <= 0 || !gOrigCb) return;
    typedef void (*l2cap_cb_t)(IOService *, IOBluetoothL2CAPChannel *, unsigned short, void *);
    ((l2cap_cb_t)gOrigCb)(gOrigTarget, channel, (unsigned short)n, mt1);
}

void com_schmonz_MT2BTReader::incomingData(IOService *target,
                                           IOBluetoothL2CAPChannel *channel,
                                           unsigned short length, void *data) {
    (void)channel;
    const uint8_t *b = (const uint8_t *)data;
    if (!b || length < 2) return;
    const uint8_t *report = b;
    size_t rlen = length;
    if (b[0] == 0xA1) { report = b + 1; rlen = length - 1; }   /* strip transport byte */
    touch_frame_t tf;
    if (mt2_bt_decode(report, rlen, &tf) != 0) return;
    if (gActiveMT2Gesture) gActiveMT2Gesture->submitFrame(target, &tf);   /* target = this reader */
}

/* Runs in-gate (the channel's Bluetooth workloop): the only place IOBluetoothFamily
 * allows IOBluetoothObject calls. arg0 is the reader. */
IOReturn com_schmonz_MT2BTReader::setupInGate(OSObject * /*owner*/, void *arg0,
                                              void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;

    unsigned short psm = self->fChannel->getPSM();

    /* Arm the shared session from bind time: it owns the settle window (drops the
     * connect-transition contact burst) and all post-decode logic. Only the INTERRUPT
     * channel (PSM 19 = 0x13) delivers touch frames, so only it registers as the
     * session's active frame source. BT binds two L2CAP channels (control 0x11 +
     * interrupt 0x13) as two separate reader instances; if the control reader claimed
     * the source instead, the session's single-active guard would reject the interrupt
     * reader's frames (== dead cursor). The control channel only sends the enable below. */
    if (psm == 0x13 && gActiveMT2Gesture)
        gActiveMT2Gesture->connectionEstablished(self, MT2_EVENT_DRIVEN);

    /* Listen on the INTERRUPT channel (PSM 19) — EXCEPT under Path A, where Apple's genuine
     * BNBTrackpadDevice must own this delegate (its handleStart listenAt's it; if we hold it,
     * BNB's listenAt returns 0xe00002bc -> forced teardown -> panic, findings §S2.5/§S2.6). */
    if (psm == 0x13 && !kGenuinePathA)
        self->fChannel->listenAt(self, &com_schmonz_MT2BTReader::incomingData);

    /* Enable multitouch streaming on the CONTROL channel only (PSM 17 = 0x11). The
     * MT2's Bluetooth enable is feature report 0xF1 {0xF1,0x02,0x01} (confirmed via
     * IOHIDDeviceSetReport). Over raw L2CAP a SET_REPORT(feature) is the HIDP
     * transaction byte (SET_REPORT<<4)|FEATURE = 0x53 then the report. */
    self->fIsControl = (psm == 0x11);
    if (psm == 0x11) {
        /* Keep sending the MT2's 0xF1 multitouch enable — a genuine BNBTrackpadDevice only knows
         * the MT1 0xD7 enable, which the MT2 ignores, so WITHOUT this the pad never streams. */
        static const uint8_t kEnable[] = { 0x53, 0xF1, 0x02, 0x01 };
        self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
    }
    IOLog("MT2BTReader: setup on PSM=%u (enable sent=%d)\n", psm, psm == 0x11);
    return kIOReturnSuccess;
}

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    fIsControl = false;
    fManualBnb = 0;
    fInterposeTimer = 0;
    fInterposeTries = 0;

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;

    /* IOBluetoothFamily REQUIREs every IOBluetoothObject call to run inside its
     * workloop gate (mWorkLoop->inGate()) — calling sendTo/listenAt directly from this
     * start() thread panics ("NOT called in IOWorkLoop"). Marshal onto the channel's
     * own command gate, which enters that workloop. */
    IOCommandGate *gate = fChannel->getCommandGate();
    if (!gate) {
        IOLog("MT2BTReader: channel has no command gate; cannot enable\n");
        return false;
    }
    gate->runAction(&com_schmonz_MT2BTReader::setupInGate, this);

    /* PATH A (S2.2c): on the control channel, manually build a genuine BNBTrackpadDevice on this
     * real L2CAP channel — bypassing IOKit matching (which can't be tricked). Done from this normal
     * start() thread, NOT in the channel command gate: the BT-HID start chain can block waiting for
     * channel handshakes, and blocking while holding the gate would deadlock. start() runs the full
     * IOHIDDevice chain on a real provider, so the IOHIDDevice state initializes (no publish-handler
     * panic). Best-effort; failure leaves our reader untouched. */
    if (kGenuinePathA && fIsControl) {
        OSObject *bo = OSMetaClass::allocClassWithName("BNBTrackpadDevice");
        IOService *bnb = OSDynamicCast(IOService, bo);
        if (!bnb) {
            IOLog("MT2BTReader: allocClassWithName(BNBTrackpadDevice) NULL (AppleBluetoothMultitouch loaded?)\n");
            if (bo) bo->release();
        } else {
            OSDictionary *props = OSDictionary::withCapacity(2);   /* non-null: init bails on NULL props */
            bool ok = props && bnb->init(props);
            if (props) props->release();
            if (ok && bnb->attach(fChannel)) {
                if (bnb->start(fChannel)) {
                    fManualBnb = bnb;
                    gGenuineBnb = bnb;   /* publish for the interrupt reader + MT2Gesture sink (Phase 2) */
                    IOLog("MT2BTReader: MANUAL genuine BNBTrackpadDevice start OK (FORM milestone)\n");
                } else {
                    IOLog("MT2BTReader: manual BNBTrackpadDevice start() FAILED\n");
                    bnb->detach(fChannel);
                    bnb->release();
                }
            } else {
                IOLog("MT2BTReader: manual BNBTrackpadDevice init/attach FAILED (init=%d)\n", ok);
                bnb->release();
            }
        }

        if (fManualBnb) {
            /* BNB's interrupt channel arrives ASYNCHRONOUSLY (publish notification), so poll
             * for it on our workloop and install the interpose once it (and BNB's listenAt)
             * are in place. */
            fInterposeTries = 0;
            IOWorkLoop *wl = getWorkLoop();
            if (wl) {
                fInterposeTimer = IOTimerEventSource::timerEventSource(
                    this, &com_schmonz_MT2BTReader::interposeTimerFired);
                if (fInterposeTimer && wl->addEventSource(fInterposeTimer) == kIOReturnSuccess)
                    fInterposeTimer->setTimeoutMS(100);
            }
        }
    }

    IOLog("MT2BTReader: bound L2CAP channel, enabled multitouch (0xF1), listening\n");
    registerService();
    return true;
}

/* In-gate: null our listenAt callback. newDataIn bails when the callback (channel+0x110)
 * is NULL before it derefs the target (channel+0x118), so this makes it safe for the
 * channel to outlive us. Same-target listenAt(self, NULL) is accepted by the family. */
IOReturn com_schmonz_MT2BTReader::teardownInGate(OSObject * /*owner*/, void *arg0,
                                                 void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    /* Clear our delegate only on the channel we actually set it on (the interrupt reader, PSM 19).
     * The control reader never set a delegate (the genuine BNBTrackpadDevice owns PSM 17's), so
     * clearing it there would clobber Apple's delegate on our unload. */
    if (self && !self->fIsControl && self->fChannel) self->fChannel->listenAt(self, 0);
    return kIOReturnSuccess;
}

/* In the channel's command gate: read BNB's current delegate callback (channel+0x110) and,
 * if BNB has registered it, swap in our shim (saving the original for the shim to forward to).
 * We overwrite ONLY the callback pointer; the target slot (+0x118) keeps BNB's own target,
 * which the shim forwards via gOrigTarget. newDataIn re-reads +0x110 per packet, so the swap
 * takes effect immediately. Returns NotReady until BNB's listenAt has populated the slot. */
IOReturn com_schmonz_MT2BTReader::interposeInGate(OSObject * /*owner*/, void *arg0,
                                                  void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    uint8_t *c = (uint8_t *)ch;
    void **cbslot = (void **)(c + L2CAP_DELEGATE_CB_OFF);
    void *cur = *cbslot;
    if (!cur || cur == (void *)&bt_interpose_shim) return kIOReturnNotReady;  /* not set yet / already ours */
    gOrigCb = cur;
    gOrigTarget = (IOService *)*(void **)(c + L2CAP_DELEGATE_CB_OFF + 8);      /* +0x118 target */
    *cbslot = (void *)&bt_interpose_shim;
    gInterposedChannel = ch;
    IOLog("MT2BTReader: Path A interpose installed (origCb=%p origTgt=%p)\n", gOrigCb, gOrigTarget);
    return kIOReturnSuccess;
}

/* In-gate: restore BNB's original callback so the channel never calls our (freed) shim. */
IOReturn com_schmonz_MT2BTReader::restoreInGate(OSObject * /*owner*/, void *arg0,
                                                void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    void **cbslot = (void **)((uint8_t *)ch + L2CAP_DELEGATE_CB_OFF);
    if (*cbslot == (void *)&bt_interpose_shim && gOrigCb) *cbslot = gOrigCb;
    return kIOReturnSuccess;
}

/* Poll for BNB's interrupt channel (gGenuineBnb+0xf0); install the interpose in that channel's
 * gate once present. Re-arm up to ~5 s, then give up. */
void com_schmonz_MT2BTReader::interposeTimerFired(OSObject *owner, IOTimerEventSource *ts) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)owner;
    IOService *bnb = gGenuineBnb;
    if (bnb) {
        IOBluetoothL2CAPChannel *ch =
            *(IOBluetoothL2CAPChannel **)((uint8_t *)bnb + BNB_INTERRUPT_CHANNEL_OFF);
        if (ch) {
            IOCommandGate *gate = ((IOBluetoothObject *)ch)->getCommandGate();
            if (gate && gate->runAction(&com_schmonz_MT2BTReader::interposeInGate, ch)
                          == kIOReturnSuccess) {
                return;  /* installed; stop polling */
            }
        }
    }
    if (++self->fInterposeTries < 50) ts->setTimeoutMS(100);
    else IOLog("MT2BTReader: Path A interpose gave up (no BNB interrupt channel)\n");
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    /* Deregister our incoming-data callback in-gate BEFORE we can be freed, or the
     * channel's newDataIn will dereference a dangling pointer (use-after-free panic
     * seen when the installer unloaded the live kext while the trackpad streamed). */
    if (fChannel) {
        IOCommandGate *gate = fChannel->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::teardownInGate, this);
    }
    /* Path A: stop polling and restore BNB's original delegate callback on its interrupt channel,
     * so the channel never calls our (about-to-be-freed) shim. Restore goes through the channel's
     * own command gate, exactly like the install. */
    if (fInterposeTimer) {
        fInterposeTimer->cancelTimeout();
        if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fInterposeTimer);
        fInterposeTimer->release();
        fInterposeTimer = 0;
    }
    if (gInterposedChannel) {
        IOCommandGate *gate = ((IOBluetoothObject *)gInterposedChannel)->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::restoreInGate, gInterposedChannel);
        gInterposedChannel = 0;
        gOrigCb = 0; gOrigTarget = 0;
    }
    /* Path A: tear down the manually-started genuine BNBTrackpadDevice before we go away, or it
     * outlives the channel it was started on. terminate() drives the normal async IOService teardown
     * (detach from provider + stop); release() drops our retained reference. */
    if (fManualBnb) {
        gGenuineBnb = 0;   /* stop the sink forwarding into it before we tear it down */
        fManualBnb->terminate();
        fManualBnb->release();
        fManualBnb = 0;
        IOLog("MT2BTReader: manual BNBTrackpadDevice terminated + released\n");
    }
    fChannel = 0;
    IOService::stop(provider);
}
