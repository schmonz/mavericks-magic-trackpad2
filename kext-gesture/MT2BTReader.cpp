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
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include "bt_l2cap_shim.h"
#include "MT2BTReader.h"
#include "MT2Gesture.h"
#include "../src/mt2_to_mt1.h"
#include "../src/mt1_encode.h"

/* Compiled as C++ under the kext toolchain (so is mt2_bt_decode.c), so these resolve
 * with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt2_pipeline.h"   /* MT2_EVENT_DRIVEN */
#include "mt2_build_flags.h"   /* kFullBnb */
#include "amd_shim.h"          /* AppleMultitouchDevice::handleTouchFrame (full-BNB direct feed) */

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

/* PATH A (branch genuine-bnbtrackpad-citizen, design 2026-06-20-bnb-pathA-interpose-seam).
 * When true: manual-start a genuine BNBTrackpadDevice on the control channel, YIELD the
 * interrupt-channel data delegate so BNB's own listenAt succeeds (removing the §S2.5 panic
 * vector), then interpose our MT2->MT1 shim on the channel's delegate-callback slot. Default
 * false = today's behaviour (we own the delegate + feed our AppleMultitouchDevice). */
static const bool kGenuinePathA = true;

/* B1-b (findings S2.17): keep the proven hybrid input (shim → submitFrame → our fDevice drives
 * cursor + gestures), and redirect BNB's handler slot (BNB+0x1b0) to our fDevice. BNB's prefs path
 * _setMultitouchPreferences reads +0x1b0 and calls setPreferences on it (RE-confirmed) — so prefpane
 * settings land on the device actually emitting frames → controls apply. No trigger / no BNB tee
 * (would double-feed via the redirect). */
static const bool kB1Spike = false;

#define BNB_HANDLER_OFF             0x1b0   /* BNBDevice multitouch handler (AppleMultitouchDevice*) */

/* RE'd field offsets (findings §S2.6, 10.9 binaries; verify via re/ if a point release differs). */
#define L2CAP_DELEGATE_CB_OFF       0x110   /* IOBluetoothL2CAPChannel: delegate callback fn-ptr */
#define BNB_INTERRUPT_CHANNEL_OFF   0xf0    /* BNBDevice::_interruptChannel */

/* The genuine BNBTrackpadDevice the control reader manual-starts (Path A). The interpose
 * installer reads it (via +0xf0) to find BNB's interrupt channel, where it pokes our shim onto
 * the delegate-callback slot. Single device, single instance — a global mirrors the pattern of
 * gActiveMT2Gesture. NULL when no genuine device is up. */
IOService *gGenuineBnb = 0;

/* The PSM-19 (interrupt) reader instance — the session's active frame source (it calls
 * connectionEstablished(self)). Under Path A the hybrid shim feeds decoded frames back through
 * gActiveMT2Gesture->submitFrame(gInterruptReader, ...) so OUR cursor-wired AppleMultitouchDevice
 * drives the cursor + gestures, while BNB (fed the MT1 tee) keeps the genuine prefpane lit. */
static IOService *gInterruptReader = 0;

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
    const uint8_t *b = (const uint8_t *)data;
    const uint8_t *rep = b;
    size_t rlen = length;
    if (length > 0 && b[0] == 0xA1) { rep = b + 1; rlen = length - 1; }   /* strip transport byte */
    touch_frame_t tf;
    int drc = mt2_bt_decode(rep, rlen, &tf);
    { static uint32_t sn = 0; if ((sn++ % 60) == 0)
        IOLog("MT2BTReader: [diag] shim hit #%u len=%u b0=0x%02x decode=%d teeing=%d\n",
              sn, length, length ? b[0] : 0, drc, (!kB1Spike && gOrigCb) ? 1 : 0); }
    if (drc != 0) return;                                                 /* not multitouch — drop */

    /* HYBRID feed 1: drive OUR cursor-wired AppleMultitouchDevice through the shared session
     * (cursor + gestures — the same path the normal driver uses). gInterruptReader matches the
     * source connectionEstablished() armed, so the session accepts the frame. */
    if (!kFullBnb && gActiveMT2Gesture && gInterruptReader)
        gActiveMT2Gesture->submitFrame(gInterruptReader, &tf);

    /* FULL-BNB feed: drive BNB's OWN spawned AppleMultitouchDevice directly — the same proven call
     * sink_feed_frame uses on our fDevice (mt1_encode -> handleTouchFrame, NO 0xA1 transport prefix),
     * but targeting BNB's handler at +0x1b0 (its createMultitouchHandler-spawned AMD). Feeding BNB's
     * raw interrupt-channel callback (gOrigCb) instead did NOT drive the cursor: BNB's re-parse path
     * doesn't route our synthetic report to handleTouchFrame. Direct is the path proven to move the
     * cursor. (Bypasses the mt2_session conditioning — quality polish is a follow-up; movement first.) */
    if (kFullBnb) {
        void *amd = gGenuineBnb ? *(void **)((uint8_t *)gGenuineBnb + BNB_HANDLER_OFF) : 0;
        uint8_t mt1[256];
        int n = amd ? mt1_encode(&tf, mt1, sizeof(mt1), bt_uptime_ms()) : -1;
        { static uint32_t fn = 0; if ((fn++ % 60) == 0)
            IOLog("MT2BTReader: [diag] fullbnb feed bnb=%p amd=%p n=%d id=0x%02x\n",
                  gGenuineBnb, amd, n, (n > 0) ? mt1[0] : 0); }
        if (amd && n > 0) ((AppleMultitouchDevice *)amd)->handleTouchFrame(mt1, (unsigned int)n);
        return;
    }

    /* HYBRID feed 2: tee the MT1 0x28 to BNB to keep its restart-watchdog quiet. SKIPPED under B1-b:
     * there BNB+0x1b0 points at our fDevice, so teeing would route postMultitouchFrame back into
     * fDevice and double-feed it. (B1-b accepts the benign watchdog instead.) */
    if (!kB1Spike && gOrigCb) {
        uint8_t mt1[256];
        mt1[0] = 0xA1;
        int n = mt1_encode(&tf, mt1 + 1, sizeof(mt1) - 1, bt_uptime_ms());
        if (n > 0) {
            typedef void (*l2cap_cb_t)(IOService *, IOBluetoothL2CAPChannel *, unsigned short, void *);
            ((l2cap_cb_t)gOrigCb)(gOrigTarget, channel, (unsigned short)(n + 1), mt1);
        }
    }
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
    IOLog("MT2BTReader: [diag] entered setupInGate PSM=%u isInactive=%d\n",
          psm, self->fChannel->isInactive());

    /* Arm the shared session from bind time: it owns the settle window (drops the
     * connect-transition contact burst) and all post-decode logic. Only the INTERRUPT
     * channel (PSM 19 = 0x13) delivers touch frames, so only it registers as the
     * session's active frame source. BT binds two L2CAP channels (control 0x11 +
     * interrupt 0x13) as two separate reader instances; if the control reader claimed
     * the source instead, the session's single-active guard would reject the interrupt
     * reader's frames (== dead cursor). The control channel only sends the enable below. */
    if (psm == 0x13) {
        gInterruptReader = self;   /* the session source; the Path A hybrid shim feeds frames as this */
        if (gActiveMT2Gesture)
            gActiveMT2Gesture->connectionEstablished(self, MT2_EVENT_DRIVEN);
    }

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
    if (psm == 0x11 && !kFullBnb) {
        /* Keep sending the MT2's 0xF1 multitouch enable — a genuine BNBTrackpadDevice only knows
         * the MT1 0xD7 enable, which the MT2 ignores, so WITHOUT this the pad never streams. */
        static const uint8_t kEnable[] = { 0x53, 0xF1, 0x02, 0x01 };
        IOLog("MT2BTReader: [diag] before 0xF1 sendTo isInactive=%d\n", self->fChannel->isInactive());
        self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
        IOLog("MT2BTReader: [diag] after 0xF1 send, PSM17 isInactive=%d\n",
              self->fChannel->isInactive());
    }
    /* B1-drive (root cause 2026-06-21): firing 0xF1 on PSM 17 BEFORE the channel reaches OPEN makes
     * sendTo block ~14s; the device tears the link down meanwhile (it never got the genuine
     * listenAt + waitForChannelState(OPEN) acceptance) -> channel inactive -> BNB attach fails -> flap.
     * So DEFER 0xF1: let BNB's handleStart accept PSM 17 first; the phase-2 reEnable timer sends 0xF1
     * after both channels are OPEN (RE §6). */
    IOLog("MT2BTReader: setup on PSM=%u (enable sent=%d)\n", psm, psm == 0x11);
    return kIOReturnSuccess;
}

/* B1: build the OSDictionary passed to BNBTrackpadDevice::init. multitouchProperties() does
 * getProperty("DefaultMultitouchProperties"), so init's property table must carry that key with the
 * genuine config — parser-type 1000 + the MultitouchHID plugin IOCFPlugInTypes — so the AppleMultitouchDevice
 * BNB spawns is recognized by MultitouchSupport (gets a user client, drives the cursor). Mirrors the
 * BNBTrackpadDriver personality (re/plist AppleBluetoothMultitouch). Caller inits with it, then releases.
 * Returns NULL on alloc failure. */
static OSDictionary *bt_build_bnb_props(void) {
    OSDictionary *top    = OSDictionary::withCapacity(1);
    OSDictionary *mt     = OSDictionary::withCapacity(9);
    OSDictionary *plugin = OSDictionary::withCapacity(1);
    OSNumber *ptype = OSNumber::withNumber((unsigned long long)1000, 32);
    OSNumber *popts = OSNumber::withNumber((unsigned long long)47, 32);
    OSString *plpath = OSString::withCString("AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin");
    if (!top || !mt || !plugin || !ptype || !popts || !plpath) {
        if (top) top->release(); if (mt) mt->release(); if (plugin) plugin->release();
        if (ptype) ptype->release(); if (popts) popts->release(); if (plpath) plpath->release();
        return 0;
    }
    plugin->setObject("0516B563-B15B-11DA-96EB-0014519758EF", plpath);
    mt->setObject("IOCFPlugInTypes", plugin);
    mt->setObject("parser-type", ptype);
    mt->setObject("parser-options", popts);
    mt->setObject("MTHIDDevice", kOSBooleanTrue);
    mt->setObject("HIDServiceSupport", kOSBooleanTrue);
    mt->setObject("TrackpadMomentumScroll", kOSBooleanTrue);
    mt->setObject("TrackpadSecondaryClickCorners", kOSBooleanTrue);
    mt->setObject("TrackpadFourFingerGestures", kOSBooleanTrue);
    top->setObject("DefaultMultitouchProperties", mt);
    plpath->release(); ptype->release(); popts->release();
    plugin->release(); mt->release();
    return top;
}

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    fIsControl = false;
    fManualBnb = 0;
    fInterposeTimer = 0;
    fInterposeTries = 0;
    fReEnableCount = 0;

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;
    IOLog("MT2BTReader: [diag] start() PSM=%u chan->isInactive=%d\n",
          fChannel->getPSM(), fChannel->isInactive());

    /* IOBluetoothFamily REQUIREs every IOBluetoothObject call to run inside its
     * workloop gate (mWorkLoop->inGate()) — calling sendTo/listenAt directly from this
     * start() thread panics ("NOT called in IOWorkLoop"). Marshal onto the channel's
     * own command gate, which enters that workloop. */
    IOCommandGate *gate = fChannel->getCommandGate();
    if (!gate) {
        IOLog("MT2BTReader: channel has no command gate; cannot enable\n");
        return false;
    }
    IOLog("MT2BTReader: [diag] calling setupInGate via runAction (PSM=%u isInactive=%d)\n",
          fChannel->getPSM(), fChannel->isInactive());
    gate->runAction(&com_schmonz_MT2BTReader::setupInGate, this);
    IOLog("MT2BTReader: [diag] runAction(setupInGate) returned (isInactive=%d)\n",
          fChannel->isInactive());

    /* PATH A (manual-start the genuine BNBTrackpadDevice + interpose translation): on the control
     * channel, manually build a genuine BNBTrackpadDevice on this
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
            /* B1: pass genuine DefaultMultitouchProperties so BNB's spawned AppleMultitouchDevice is
             * recognized + drives the cursor (findings S2.17). Hybrid (no B1) only needs a non-null dict. */
            OSDictionary *props = (kB1Spike || kFullBnb) ? bt_build_bnb_props() : OSDictionary::withCapacity(2);
            bool ok = props && bnb->init(props);
            if (props) props->release();
            if (ok) IOLog("MT2BTReader: [diag] pre-attach chan->isInactive=%d\n", fChannel->isInactive());
            if (ok && bnb->attach(fChannel)) {
                if (bnb->start(fChannel)) {
                    fManualBnb = bnb;
                    gGenuineBnb = bnb;   /* publish for the interrupt reader + MT2Gesture sink (Phase 2) */
                    /* B1-b: point BNB's handler slot at our fDevice, so BNB's prefs path
                     * (_setMultitouchPreferences → +0x1b0 → setPreferences) lands prefpane settings on
                     * the device that actually emits frames. No trigger → BNB makes no AMD of its own. */
                    if (kB1Spike && gActiveMT2Gesture && gActiveMT2Gesture->rawDevice()) {
                        *(void **)((uint8_t *)bnb + BNB_HANDLER_OFF) = gActiveMT2Gesture->rawDevice();
                        IOLog("MT2BTReader: B1-b — redirected BNB+0x1b0 to our fDevice (%p)\n",
                              gActiveMT2Gesture->rawDevice());
                    }
                    IOLog("MT2BTReader: Path A manual BNBTrackpadDevice start OK\n");
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
                if (fInterposeTimer) {
                    if (wl->addEventSource(fInterposeTimer) == kIOReturnSuccess)
                        fInterposeTimer->setTimeoutMS(100);
                    else { fInterposeTimer->release(); fInterposeTimer = 0; }   /* invariant: fInterposeTimer != 0 => attached */
                }
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
/* In the CONTROL channel's command gate: re-send the MT2's 0xF1 multitouch enable (same
 * HIDP SET_REPORT(feature) payload setupInGate sends). BNB's handleStart leaves the device in
 * mouse mode (report 0x02) after our initial enable, so this forces it back to multitouch
 * (report 0x31). arg0 = the control reader; its fChannel is the PSM-17 control channel. */
IOReturn com_schmonz_MT2BTReader::reEnableInGate(OSObject * /*owner*/, void *arg0,
                                                 void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;
    static const uint8_t kEnable[] = { 0x53, 0xF1, 0x02, 0x01 };
    self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
    return kIOReturnSuccess;
}

/* B1-drive probe: inject the 0x60/0x02 handler-create trigger into BNB's own data callback (gOrigCb),
 * which we saved when installing the interpose. Runs in the interrupt channel's gate — the same
 * context BNB's real data callback runs in. arg0 = the interposed interrupt channel. */
IOReturn com_schmonz_MT2BTReader::triggerInGate(OSObject * /*owner*/, void *arg0,
                                                void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    /* 0xA1 transport byte + reportID 0x60 + 0x02, zero-padded. The minimal padded payload is enough to
     * drive BNB's processDesyncedMultitouchData -> startMultitouch -> createMultitouchHandler (S2.17,
     * no body-parse fault). */
    static const uint8_t kTrigger[16] = { 0xA1, 0x60, 0x02 };
    typedef void (*l2cap_cb_t)(IOService *, IOBluetoothL2CAPChannel *, unsigned short, void *);
    if (gOrigCb) {
        ((l2cap_cb_t)gOrigCb)(gOrigTarget, ch, (unsigned short)sizeof(kTrigger), (void *)kTrigger);
        IOLog("MT2BTReader: B1-drive injected 0x60/0x02 handler-create trigger into BNB\n");
    }
    return kIOReturnSuccess;
}

void com_schmonz_MT2BTReader::interposeTimerFired(OSObject *owner, IOTimerEventSource *ts) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)owner;

    /* Phase 2 (interpose already installed): re-send the 0xF1 enable on the control channel a
     * few times to force the device back into multitouch mode after BNB's handleStart knocked
     * it to mouse mode (findings S2.11). self->fChannel is the control channel (this is the
     * control reader — the manual-start + this timer are armed only when fIsControl). */
    if (gInterposedChannel) {
        if (self->fChannel) {
            IOCommandGate *cg = ((IOBluetoothObject *)self->fChannel)->getCommandGate();
            if (cg) cg->runAction(&com_schmonz_MT2BTReader::reEnableInGate, self);
        }
        if (++self->fReEnableCount < 8) ts->setTimeoutMS(250);  /* ~2 s of re-enables, then stop */
        else IOLog("MT2BTReader: Path A re-enable phase done (%d sends)\n", self->fReEnableCount);
        return;
    }

    /* Phase 1: poll for BNB's interrupt channel; install the interpose once present. */
    IOService *bnb = gGenuineBnb;
    if (bnb) {
        IOBluetoothL2CAPChannel *ch =
            *(IOBluetoothL2CAPChannel **)((uint8_t *)bnb + BNB_INTERRUPT_CHANNEL_OFF);
        if (ch) {
            IOCommandGate *gate = ((IOBluetoothObject *)ch)->getCommandGate();
            if (gate && gate->runAction(&com_schmonz_MT2BTReader::interposeInGate, ch)
                          == kIOReturnSuccess) {
                self->fReEnableCount = 0;
                /* B1-drive: interpose is in (gOrigCb = BNB's data cb). Inject the handler-create
                 * trigger ONCE, in this same channel gate, so BNB spawns its own AMD before the
                 * re-enable phase. (gInterposedChannel is now set; this success branch runs once.) */
                if (kFullBnb)
                    gate->runAction(&com_schmonz_MT2BTReader::triggerInGate, ch);
                ts->setTimeoutMS(250);  /* installed → enter phase 2 (re-enable multitouch) */
                return;
            }
        }
    }
    if (++self->fInterposeTries < 50) ts->setTimeoutMS(100);
    else IOLog("MT2BTReader: Path A interpose gave up (no BNB interrupt channel)\n");
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    /* If we're the interrupt reader that armed the session source, clear it so the Path A
     * hybrid shim stops feeding through a freed instance. */
    if (gInterruptReader == this) gInterruptReader = 0;
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
        /* cancelTimeout() does NOT wait for an already-running handler, so release() could
         * otherwise free the timer mid-fire. interposeTimerFired runs under the reader's
         * workloop gate (IOTimerEventSource fires its action under the gate of the workloop
         * it's attached to — here getWorkLoop()), and IOWorkLoop::removeEventSource closes
         * that same gate — so it BLOCKS until any in-flight handler returns before removing
         * the source, making the subsequent release() safe. Order: cancel -> remove -> release. */
        fInterposeTimer->cancelTimeout();
        if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fInterposeTimer);
        fInterposeTimer->release();
        fInterposeTimer = 0;
    }
    /* Restore is keyed on the file-static global gInterposedChannel (not self) — correct for the
     * single-genuine-device design (mirrors gGenuineBnb): only one device is ever interposed. */
    if (gInterposedChannel) {
        IOCommandGate *gate = ((IOBluetoothObject *)gInterposedChannel)->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::restoreInGate, gInterposedChannel);
        gInterposedChannel = 0;
        gOrigCb = 0; gOrigTarget = 0;
    }
    /* Path A: tear down the manually-started genuine BNBTrackpadDevice before we go away, or it
     * outlives the channel it was started on.
     *
     * SYNCHRONOUS teardown (wedge/flap root-cause probe, 2026-06-21): plain terminate() is ASYNC and
     * unwaited, so BNB's own teardown (its interrupt-channel listenAt(NULL)+closeChannel+release, per
     * genuine closeDownServicesWL) races kext unload -> the L2CAP channel is left half-torn -> kextunload
     * reports "busy" (Error 3) and the NEXT load can't bind (findings §S2.9 "reboot between iterations").
     * The SAME incomplete teardown is the likely cause of attach failing on BT reconnect (§S2.14). So:
     * terminate() then waitQuiet() drive BNB's teardown to quiescence BEFORE we release + return, leaving
     * the channel clean for the next bind. Bounded to 2 s so a stuck teardown can't hang the unload; the
     * logged waitQuiet return (0 == quiesced, 0xe00002d6 == kIOReturnTimeout) is the probe's key signal. */
    if (fManualBnb) {
        gGenuineBnb = 0;   /* stop the sink forwarding into it before we tear it down */
        fManualBnb->terminate();
        IOReturn wq = fManualBnb->waitQuiet(2ULL * 1000 * 1000 * 1000);
        fManualBnb->release();
        fManualBnb = 0;
        IOLog("MT2BTReader: manual BNBTrackpadDevice terminate+waitQuiet(=0x%x)+release\n", wq);
    }
    fChannel = 0;
    IOService::stop(provider);
}
