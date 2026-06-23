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
#include "mt2_build_flags.h"   /* kFullBnb, kBnbGeometry */
#include "mt2_log.h"           /* MT2_DLOG (runtime debug.mt2_log) */
#include "../src/conn_trace.h" /* CONNTRACE emitter (connect-flap measurement) */
#include "amd_shim.h"          /* AppleMultitouchDevice::handleTouchFrame (full-BNB direct feed) */

extern "C" {
#include "mt2_geometry.h"      /* single source of the sensor-geometry D-report payloads */
}
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "vtable_clone.h"      /* instance-scoped vtable clone/override/restore */

/* getMultitouchReport vtable slot. staticGetReportHandler (AppleBluetoothMultitouch @0x1305)
 * tail-calls transport->vtable[0xcc8](transport, reportId, buf, &len, count) — instruction-verified
 * on 10.9. BNBDevice does NOT override this slot, so overriding it on OUR transport instance
 * short-circuits the AMD's geometry query before it descends to BNBDevice::_getMultitouchReport
 * (slot 0xd58, dispatched via the gate-runner at 0xd78). Confirm on the target build in the plan's
 * Task 1; if it differs there, change this one constant. */
#define GMR_SLOT_INDEX    (0xcc8 / sizeof(void *))   /* getMultitouchReport     — DATA fetch */
#define GMRINFO_SLOT_INDEX (0xcd8 / sizeof(void *))  /* getMultitouchReportInfo — LENGTH probe, runs FIRST */
/* Clone span: must cover the highest dispatched slot (>= 0xd78) + margin. Generous fixed span. */
#define BNB_VTABLE_SPAN  0x2000

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

/* Full-BNB geometry (kBnbGeometry): our transport instance's cloned vtable. */
static vtc_clone_t gBnbVtableClone;
static bool        gBnbVtableCloned = false;

/* Replacement for BluetoothMultitouchTransport::getMultitouchReport on OUR transport instance.
 * staticGetReportHandler tail-calls this as
 *   getMultitouchReport(transport, uchar reportId, uchar* buf, uint* len, int count)
 * (buf = reportStruct+1, len = reportStruct+0x204). We answer the geometry D-reports from the
 * shared source and report every other id as unsupported — so BNB's AMD's bring-up
 * cacheDeviceProperties publishes real geometry and the MTDevice is born with correct dims. */
extern "C" int mt2_bnb_get_multitouch_report(void *transport, unsigned char reportId,
                                             unsigned char *buf, unsigned int *len, int count) {
    (void)transport; (void)count;
    unsigned int n = 0;
    if (mt2_fill_geometry_report(reportId, buf, &n) == MT2_GEO_OK) {
        if (len) *len = n;
        MT2_DLOG(2, "BNB geometry GET id=0x%02x -> %u bytes", (unsigned)reportId, n);
        return 0;
    }
    return (int)0xe00002c7;   /* kIOReturnUnsupported (matches the original stub for non-geometry) */
}

/* getMultitouchReportInfo override (transport vtable slot 0xcd8). _deviceGetReportWithLookUp probes
 * the report LENGTH via this BEFORE the data fetch; the original transport stub returns unsupported,
 * which short-circuits the whole query before getMultitouchReport (0xcc8) is ever reached. We answer
 * the geometry ids with their payload length so the flow proceeds to the data fetch. Same signature
 * (transport, reportId, buf, uint* len, count) per ABM staticReportInfoHandler @0x1356. */
extern "C" int mt2_bnb_get_multitouch_report_info(void *transport, unsigned char reportId,
                                                  unsigned char *buf, unsigned int *len, int count) {
    (void)transport; (void)buf; (void)count;
    unsigned char tmp[64];
    unsigned int n = 0;
    if (mt2_fill_geometry_report(reportId, tmp, &n) == MT2_GEO_OK) {
        if (len) *len = n;
        MT2_DLOG(2, "BNB geometry INFO id=0x%02x -> %u bytes", (unsigned)reportId, n);
        return 0;
    }
    return (int)0xe00002c7;
}

static uint32_t bt_uptime_ms(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint32_t)(s * 1000 + u / 1000);
}

/* Connect-flap measurement: each connection attempt gets an id (bumped when a control channel
 * comes up), and we emit one canonical CONNTRACE line per observed transition through the SHARED
 * conn_trace_format() (so the kernel emit and re/conn-trace parsing can't drift). Gated at
 * debug.mt2_log>=1; `re/conn-trace <klog>` renders the per-connection timeline + STEADY/FAIL
 * verdict. Pure observation of the CURRENT flow — not yet the state-machine-driven connect. */
static int gConnId = 0;
static void bt_conntrace(csm_state_t st, csm_event_t ev, const void *chan,
                         const void *bnb, const void *deleg, int ret) {
    if (gMT2LogLevel < 1) return;   /* skip formatting when diagnostics are off */
    conn_trace_rec_t r;
    r.ts_ms = bt_uptime_ms(); r.conn_id = gConnId;
    r.state = st; r.event = ev;
    r.chan = chan; r.bnb = bnb; r.deleg = deleg; r.ret = ret;
    char buf[192];
    if (conn_trace_format(buf, sizeof(buf), &r) > 0) MT2_DLOG(1, "%s", buf);
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
    { static bool once = false; if (!once) { once = true;
        IOLog("MT2BTReader: [diag] first shim hit len=%u b0=0x%02x decode=%d\n",
              length, length ? b[0] : 0, drc); } }
    if (drc != 0) return;                                                 /* not multitouch — drop */

    /* STEADY: first real multitouch frame of this connection = the pad is streaming end-to-end. */
    { static int gSteadyConn = -1; if (gSteadyConn != gConnId) { gSteadyConn = gConnId;
        bt_conntrace(CSM_STEADY, CSM_EV_FRAME, channel, gGenuineBnb, 0, 0); } }

    /* FULL-BNB feed: route through the SHARED mt2_session — the same conditioning the smooth
     * fDevice path uses (drop-lifted, MakeTouch/Touching/BreakTouch lifecycle, liftoff) — but
     * with the session sink targeting BNB's own AMD (setBnbTarget). The previous direct feed
     * (raw mt1_encode -> handleTouchFrame, no lifecycle states) janked the cursor everywhere:
     * the recognizer never saw proper contact-start/lift transitions. Reuses the proven path. */
    if (kFullBnb) {
        if (gActiveMT2Gesture && gInterruptReader) {
            void *amd = gGenuineBnb ? *(void **)((uint8_t *)gGenuineBnb + BNB_HANDLER_OFF) : 0;
            gActiveMT2Gesture->setBnbTarget(amd);
            gActiveMT2Gesture->submitFrame(gInterruptReader, &tf);
        }
        return;
    }

    /* HYBRID feed 1: drive OUR cursor-wired AppleMultitouchDevice through the shared session
     * (cursor + gestures — the same path the normal driver uses). gInterruptReader matches the
     * source connectionEstablished() armed, so the session accepts the frame. */
    if (gActiveMT2Gesture && gInterruptReader)
        gActiveMT2Gesture->submitFrame(gInterruptReader, &tf);

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
        bt_conntrace(CSM_INTERRUPT_BOUND, CSM_EV_INTERRUPT_PUBLISHED, self->fChannel, 0, 0, 0);
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
    /* A control channel coming up starts a new connection attempt — bump the id and mark CONTROL_UP. */
    if (psm == 0x11) { gConnId++; bt_conntrace(CSM_CONTROL_UP, CSM_EV_CONTROL_OPEN, self->fChannel, 0, 0, 0); }
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
    /* Physical-click reliability: BNB's createMultitouchHandler copies these DefaultMultitouchProperties
     * keys onto the spawned AMD before AMD::start, which reads getProperty("ExtractAndPostDeviceButtonState")
     * to set the S+9 device-button gate. Without it, handlePointerEventFromDevice (our click sink) posts
     * are dropped on BNB's AMD -> only recognizer tap-to-click survives. Mirrors the fDevice path
     * (MT2Gesture.cpp setProperty before start). */
    mt->setObject("ExtractAndPostDeviceButtonState", kOSBooleanTrue);
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
                /* Geometry override must be live BEFORE start()/the AMD bring-up's first
                 * _cacheDeviceProperties query, or the MTDevice is born with empty geometry and
                 * the late timer-install is never consulted. Install here (idempotent; the later
                 * pre-trigger call no-ops). If start() fails we restore before release. */
                if (kFullBnb) installBnbGeometry(bnb);
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
                    bt_conntrace(CSM_BNB_FORMED, CSM_EV_BNB_LISTENING, fChannel, bnb, 0, 0);
                } else {
                    IOLog("MT2BTReader: manual BNBTrackpadDevice start() FAILED\n");
                    if (kFullBnb) removeBnbGeometry(bnb);   /* restore vtable before freeing */
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
    bt_conntrace(CSM_INTERPOSED, CSM_EV_INTERPOSE_OK, ch, gGenuineBnb, gOrigCb, 0);
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

/* Clone OUR transport instance's vtable and override the getMultitouchReport slot with our
 * geometry answerer. Must run BEFORE the create trigger so BNB's AMD's bring-up query is answered.
 * Instance-scoped: only this transport's vtable pointer changes; the shared class vtable (and a
 * co-connected genuine MT1's transport) is untouched. */
void com_schmonz_MT2BTReader::installBnbGeometry(void *transport) {
    if (!kBnbGeometry || !transport || gBnbVtableCloned) return;
    /* Safety gate: we clone THIS object's vtable and write slot 0xcc8, valid only if it really is
     * the BNBTrackpadDevice transport (its vtable has getMultitouchReport there). If the handle is
     * ever something else, cloning + slot-write would corrupt an unrelated method -> panic. Verify
     * the class name and ABORT rather than clone blind. (gGenuineBnb is set from allocClassWithName
     * "BNBTrackpadDevice"; this also catches an offset/handle regression.) */
    const char *cls = ((IOService *)transport)->getMetaClass()->getClassName();
    if (!cls || (strcmp(cls, "BNBTrackpadDevice") != 0 &&
                 strcmp(cls, "BluetoothMultitouchTransport") != 0)) {
        IOLog("MT2BTReader: BNB geometry ABORT — transport %p is '%s', not BNBTrackpadDevice\n",
              transport, cls ? cls : "(null)");
        return;
    }
    MT2_DLOG(1, "BNB geometry target class = %s", cls);
    if (vtc_clone_override(transport, BNB_VTABLE_SPAN, GMR_SLOT_INDEX,
                           (void *)&mt2_bnb_get_multitouch_report, &gBnbVtableClone) == 0) {
        /* The geometry query probes report length via getMultitouchReportInfo (0xcd8) FIRST; if that
         * fails it never reaches getMultitouchReport (0xcc8). Override both on the same clone. */
        vtc_override_slot(&gBnbVtableClone, GMRINFO_SLOT_INDEX,
                          (void *)&mt2_bnb_get_multitouch_report_info);
        gBnbVtableCloned = true;
        MT2_DLOG(1, "BNB geometry override installed on transport %p (data slot %lu, info slot %lu)",
                 transport, (unsigned long)GMR_SLOT_INDEX, (unsigned long)GMRINFO_SLOT_INDEX);
    } else {
        IOLog("MT2BTReader: BNB geometry override FAILED to allocate\n");
    }
}

void com_schmonz_MT2BTReader::removeBnbGeometry(void *transport) {
    if (!gBnbVtableCloned) return;
    vtc_restore(transport, &gBnbVtableClone);
    gBnbVtableCloned = false;
    IOLog("MT2BTReader: BNB geometry override removed\n");
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
        /* HANDLER_UP once the trigger has spawned BNB's AMD (visible at gGenuineBnb+0x1b0). */
        if (gGenuineBnb) {
            void *amd = *(void **)((uint8_t *)gGenuineBnb + BNB_HANDLER_OFF);
            static int gHandlerConn = -1;
            if (amd && gHandlerConn != gConnId) { gHandlerConn = gConnId;
                bt_conntrace(CSM_HANDLER_UP, CSM_EV_HANDLER_SPAWNED, 0, gGenuineBnb, amd, 0); }
        }
        if (++self->fReEnableCount < 8) ts->setTimeoutMS(250);  /* ~2 s of re-enables, then stop */
        else {
            IOLog("MT2BTReader: Path A re-enable phase done (%d sends)\n", self->fReEnableCount);
            bt_conntrace(CSM_MT_MODE, CSM_EV_MT_MODE_CONFIRMED, self->fChannel, gGenuineBnb, 0, 0);
        }
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
                /* Override OUR transport's getMultitouchReport BEFORE the trigger, so the AMD the
                 * trigger is about to spawn answers its bring-up geometry query from our constants
                 * (the MTDevice is born with correct dims; a late install would be too late). */
                if (kFullBnb)
                    self->installBnbGeometry(gGenuineBnb);
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
    bt_conntrace(CSM_IDLE, CSM_EV_DISCONNECT, fChannel, fManualBnb, 0, 0);
    /* If we're the interrupt reader that armed the session source, clear it so the Path A
     * hybrid shim stops feeding through a freed instance. */
    if (gInterruptReader == this) gInterruptReader = 0;
    /* Stop the full-BNB session sink from feeding a now-stale BNB AMD pointer. */
    if (gActiveMT2Gesture) gActiveMT2Gesture->setBnbTarget(0);
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
        /* Restore the transport's original vtable pointer (and free the clone) BEFORE terminate,
         * so BNB tears down through Apple's own code, not our override. */
        removeBnbGeometry(fManualBnb);
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
