/* MT2BTReader — the Bluetooth transport reader for the Magic Trackpad 2.
 *
 * Two halves. PER-TRANSPORT (the ~3%): bind the L2CAP channels, decode each raw MT2 0x31
 * frame (mt2_bt_decode), and declare BT config — sensor geometry, the 0xF1 enable, battery
 * poll. SHARED INTERFACE (the ~97%): this reader is a VoodooInput SATELLITE — on interrupt
 * bind it advertises VoodooInputSupported + its coordinate span and registerService()s; the
 * mux (com_schmonz_MavericksVoodooInput) attaches as our client. incomingData decodes to a MavericksTouchFrame,
 * mavericks_voodoo_from_frame's it to a VoodooInputEvent, and messageClient()s the mux, which owns
 * the terminal AMD + conditioning. No BNBTrackpadDevice is ever started; no fabricated AMD
 * is built here. No decision logic lives in this file.
 *
 * Load-bearing RE prose is in docs/mt-stack/explanation.md; the code keeps one-line pointers.
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
#include "MavericksVoodooInputHost.h"

/* Compiled as C++ under the kext toolchain (so is mt2_bt_decode.c), so these resolve
 * with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt2_battery.h"    /* mt2_parse_battery_report — shared pure decode of report 0x90 */
#include "mavericks_pipeline.h"   /* MAVERICKS_EVENT_DRIVEN */
#include "mavericks_log.h"           /* MAVERICKS_DLOG (runtime debug.mavericks_log) */
#include "mavericks_diag.h"          /* shared per-transport stream diagnostics (report id / first frame / edge / gap) */
#include "../src/mavericks_conn_trace.h" /* CONNTRACE emitter (connect-flap measurement) */
#include "../src/mavericks_stack.h"  /* canonical RE facts: vtable slots, field offsets, props */
#include "../src/mavericks_coordinator.h"  /* transport-coordinator seam (no-op for MT2) */
#include "mavericks_voodoo_translate.h"     /* mavericks_voodoo_from_frame (satellite emit) + MT2_SPAN_* via mt2_coord_range.h */
#include "voodoo_wire.h"              /* VoodooInputEvent + VOODOO_INPUT_* keys + kIOMessageVoodooInputMessage */
#include "MavericksVoodooInput.h"           /* com_schmonz_MavericksVoodooInput::publishBattery() — hands battery to the backend */
#include "../src/mt2_coord_range.h"   /* MT2_SPAN_X / MT2_SPAN_Y (advertised logical max + emit scaling) */
/* mavericks_splice_kext.h -> mavericks_splice.h -> vtable_clone.h requires these macros before the include. */
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "mavericks_splice_kext.h"   /* declarative interpose engine + kext ops (mavericks_splice_kext_ops). */

/* Battery poll cadence (ms). The MT2 only answers a GET_REPORT(0x90) — it never streams battery —
 * so the control reader polls on this interval once the connection is settled. A battery moves
 * slowly; 30 s keeps the prefpane number fresh without churning the control channel. */
#define MAVERICKS_BATTERY_POLL_MS  30000

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

/* Field offsets: canonical values + re/ commands live in ../src/mavericks_stack.h. These are readable
 * local aliases so the numbers exist in exactly one place (no doc/build drift). */
#define L2CAP_DELEGATE_CB_OFF       MAVERICKS_OFF_L2CAP_DELEGATE_CB     /* L2CAP delegate cb (+8 = target)*/

/* The bound VoodooInput mux (set by the interrupt reader once it locates its attached mux).
 * The mux owns the terminal fabricated AMD now; the control reader's battery poll publishes
 * BatteryPercent on the mux's AMD node through here. A single global (one device at a time). */
static com_schmonz_MavericksVoodooInput *gBtMux = 0;

/* The PSM-19 (interrupt) reader instance — the session's active frame source. incomingData
 * translates decoded frames to VoodooInputEvent and messages the mux. */
static IOService *gInterruptReader = 0;

/* Control-delegate seam state (battery channel). gCtrlInterposedChannel is the "which channel"
 * tracker (where to run the restore). */
static mavericks_splice_state_t gBtControlState;
static IOBluetoothL2CAPChannel *gCtrlInterposedChannel = 0;

static uint32_t bt_uptime_ms(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint32_t)(s * 1000 + u / 1000);
}

/* Connect-flap measurement: each connection attempt gets an id (bumped on control-channel open); we
 * emit one canonical CONNTRACE line per transition through the SHARED mavericks_conn_trace_format() (kernel emit
 * and `re/conn-trace` parsing can't drift). Gated at debug.mavericks_log>=1. */
static int gConnId = 0;
/* The connId whose FIRST real multitouch frame we've seen (STEADY). Set by incomingData; read by
 * interposeTimerFired to gate the enable RETRY — re-enable ONLY while gSteadyConn != gConnId. This is
 * the reconnect fix's whole teardown-safety; see explanation.md "Reconnect re-enable ... teardown-safe". */
static volatile int gSteadyConn = -1;
static void bt_conntrace(csm_state_t st, csm_event_t ev, const void *chan,
                         const void *bnb, const void *deleg, int ret) {
    if (gMavericksLogLevel < 1) return;   /* skip formatting when diagnostics are off */
    mavericks_conn_trace_rec_t r;
    r.ts_ms = bt_uptime_ms(); r.conn_id = gConnId;
    r.state = st; r.event = ev;
    r.chan = chan; r.bnb = bnb; r.deleg = deleg; r.ret = ret;
    char buf[192];
    if (mavericks_conn_trace_format(buf, sizeof(buf), &r) > 0) MAVERICKS_DLOG(1, "%s", buf);
}

/* If `data`/`len` is a battery report (0x90, optional 0xA1 transport byte), hand the capacity to the
 * terminal backend (through the located mux), which publishes it as "BatteryPercent" on the fabricated
 * AMD node IT owns — the reader no longer reaches that node. The backend applies the debug.mt2_batt
 * override, dedups, and self-fences until its AMD is up; gBtMux is NULL until the reader locates the mux.
 * The pure parse is host-tested (tests/test_battery.c). No-op when the packet isn't 0x90. */
static void mavericks_maybe_publish_battery(const void *data, size_t len) {
    uint8_t pct;
    if (!mt2_parse_battery_report((const uint8_t *)data, len, &pct)) return;
    if (gBtMux) gBtMux->publishBattery(pct);
}

/* CONTROL-channel (PSM 17) delegate shim. Unlike the interrupt shim (which consumes touch frames),
 * this is peek-and-forward: EVERY PDU must reach the saved callback unchanged (if any; without BNB
 * the slot may be unset — forward is a no-op). We only sniff GET_REPORT(0x90) responses
 * ([0xA1][0x90][flags][cap]) to log the battery %. Runs in the control channel's BT workloop. */
static void bt_control_shim(IOService *target, IOBluetoothL2CAPChannel *channel,
                            unsigned short length, void *data) {
    (void)target;
    const uint8_t *b = (const uint8_t *)data;
    /* Log distinct ids seen on control (confirmed the battery 0x90 response arrives here, not on the
     * interrupt channel). Strip the 0xA1 transport byte for the id, matching the interrupt diag.
     * saw_id (not _raw): control-plane battery polls must NOT reset the touch stream's idle-gap clock. */
    if (length > 0) mavericks_diag_saw_id(MAVERICKS_DIAG_BT, (length > 1 && b[0] == 0xA1) ? b[1] : b[0]);
    mavericks_maybe_publish_battery(data, length);
    /* Forward to saved delegate (set if there was a prior delegate, e.g. from BNB; no-op if null). */
    if (gBtControlState.saved_cb) {
        typedef void (*bt_l2cap_cb_t)(IOService *, IOBluetoothL2CAPChannel *, unsigned short, void *);
        ((bt_l2cap_cb_t)gBtControlState.saved_cb)(
            (IOService *)gBtControlState.saved_target, channel, length, data);
    }
}

/* The BT control-channel L2CAP delegate seam as a declarative row (MEM_SLOT: swap the cb at +0x110,
 * the engine saves both cb and the adjacent +0x118 target). SLOT_POPULATED gate = the old
 * "not set yet / already ours -> NotReady" precondition. */
static const mavericks_splice_row_t kBtControlRow = {
    "bt-control", MAVERICKS_SPLICE_MEM_SLOT, MAVERICKS_GATE_SLOT_POPULATED, 0, 0,
    L2CAP_DELEGATE_CB_OFF, (void *)&bt_control_shim, 0, 0, 0
};

/* Runs in-gate (the channel's Bluetooth workloop): the only place IOBluetoothFamily
 * allows IOBluetoothObject calls. arg0 is the reader. */
IOReturn com_schmonz_MT2BTReader::setupInGate(OSObject * /*owner*/, void *arg0,
                                              void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;

    unsigned short psm = self->fChannel->getPSM();
    IOLog("MT2BTReader: [diag] entered setupInGate PSM=%u isInactive=%d\n",
          psm, self->fChannel->isInactive());

    /* Arm the shared session from bind time (it owns the settle window + post-decode logic). Only the
     * INTERRUPT channel (PSM 19 = 0x13) delivers touch frames, so only it registers as the session's
     * active frame source — the two L2CAP channels bind as two reader instances, and the session's
     * single-active guard would reject the interrupt frames if the control reader claimed the source. */
    if (psm == 0x13) {   /* interrupt channel = the session's frame source */
        gInterruptReader = self;
        bt_conntrace(CSM_INTERRUPT_BOUND, CSM_EV_INTERRUPT_PUBLISHED, self->fChannel, 0, 0, 0);
        /* Become a VoodooInput satellite. Advertise support + our coordinate span, then
         * registerService so the mux (com_schmonz_MavericksVoodooInput) matches us as its provider and
         * attaches as our client. incomingData emits VoodooInputEvent to that mux; the mux owns
         * the terminal AMD + conditioning (identical MAVERICKS_EVENT_DRIVEN/mavericks_policy_default). */
        self->setProperty("VoodooInputSupported", kOSBooleanTrue);
        self->setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, (unsigned long long)MT2_SPAN_X, 32);
        self->setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, (unsigned long long)MT2_SPAN_Y, 32);
        self->setProperty("MT2 Transport", "BT");   /* mux builds a BT-transport fabricated AMD */
        /* Register OUR delegate on the interrupt channel: incomingData decodes 0x31 -> messageClient.
         * listenAt is safe here because no BNB races us for this channel (no manual-start). */
        self->fChannel->listenAt(self, &com_schmonz_MT2BTReader::incomingData);
        /* Genuine handshake step 4 (reference.md "BT connect handshake"): WAIT for the interrupt
         * channel to reach OPEN before publishing — accept-then-wait, don't race the device. */
        self->fChannel->waitForChannelState(kIOBluetoothL2CAPChannelStateOpen);
        self->registerService();   /* -> mux attaches async; fMux found lazily in incomingData */
        IOLog("MT2BTReader: VoodooInput satellite up (LMAX %u x %u)\n",
              (unsigned)MT2_SPAN_X, (unsigned)MT2_SPAN_Y);
    }

    self->fIsControl = (psm == 0x11);
    /* The 0xF1 multitouch enable stays DEFERRED to the reEnable timer (it must not precede OPEN). */
    IOLog("MT2BTReader: setup on PSM=%u\n", psm);
    /* A control channel coming up starts a new connection attempt — bump the id, mark CONTROL_UP, and
     * reset the shared diag so a reconnect re-observes report ids + re-arms the first-frame/gap markers. */
    if (psm == 0x11) {
        gConnId++; mavericks_diag_reset(MAVERICKS_DIAG_BT); bt_conntrace(CSM_CONTROL_UP, CSM_EV_CONTROL_OPEN, self->fChannel, 0, 0, 0);
        /* Genuine handshake steps 1-2 (reference.md "BT connect handshake"): ACCEPT the control channel
         * (listenAt) then WAIT for it to reach OPEN, writing no HID byte first. This acceptance is the
         * wire action that provokes the device to open its device-initiated PSM 19 interrupt channel;
         * omitting it is the ~1s flap where PSM 19 never opens. The 0xF1 enable stays deferred (below). */
        self->fChannel->listenAt(self, &com_schmonz_MT2BTReader::controlData);
        self->fChannel->waitForChannelState(kIOBluetoothL2CAPChannelStateOpen);
        /* Genuine handshake step 5 = deviceReady (reference.md "BT connect handshake"): the HID device
         * setup Apple's IOBluetoothHIDDriver does on connect that owned-BT skipped entirely —
         * SET_PROTOCOL(report) then SET_IDLE(0) on the control channel. RE'd from IOBluetoothHIDDriver:
         * setProtocol sends one byte 0x70|bit (0x70=boot, 0x71=report); the 0x5AC:0x0309 bit-inversion
         * does NOT apply to our MT2 (BT-SIG 0x4C:0x0265), so report = 0x71. setIdle sends {0x90, rate};
         * rate 0 = report-on-change. The device already streams report 0x31, so SET_PROTOCOL(report) is a
         * no-op for streaming. HYPOTHESIS (open-questions.md Layer A): completing the full HID setup is
         * what leaves the device "properly HID-connected" and thus reconnect-ready at the login screen —
         * a 0xF1-only session streams but does not arm the device's cold-boot reconnect. */
        static const uint8_t kSetProtocolReport[] = { MAVERICKS_HIDP_SET_PROTOCOL | 0x01 }; /* 0x71 */
        static const uint8_t kSetIdle[]           = { 0x90, 0x00 };                   /* SET_IDLE rate 0 */
        self->fChannel->sendTo((void *)kSetProtocolReport, sizeof(kSetProtocolReport), 0, self, 0, 0);
        self->fChannel->sendTo((void *)kSetIdle, sizeof(kSetIdle), 0, self, 0, 0);
        IOLog("MT2BTReader: deviceReady — sent SET_PROTOCOL(report 0x71) + SET_IDLE(0) on control\n");
    }
    return kIOReturnSuccess;
}

/* Direct L2CAP interrupt-channel listener (recovered from 9334273^; adapted to current decode types).
 * We register this as OUR delegate on the interrupt channel (listenAt) — no BNB needed, no interpose.
 * The genuine path COULD NOT do this (BNB's listenAt would 0xe00002bc → panic; §S2.5/§S2.6); without
 * BNB the channel is ours from the start and listenAt is clean. */
void com_schmonz_MT2BTReader::incomingData(IOService *target,
                                           IOBluetoothL2CAPChannel *channel,
                                           unsigned short length, void *data) {
    (void)channel;
    const uint8_t *b = (const uint8_t *)data;
    if (!b || length < 2) return;
    const uint8_t *report = b;
    size_t rlen = length;
    if (b[0] == 0xA1) { report = b + 1; rlen = length - 1; }   /* strip HID transport byte */

    /* Shared diag: distinct report id (once) + resume-after-gap. Mirrors bt_interpose_shim. */
    if (rlen > 0) mavericks_diag_raw(MAVERICKS_DIAG_BT, report[0]);

    MavericksTouchFrame tf;
    int drc = mt2_bt_decode(report, rlen, &tf);
    { static bool once = false; if (!once) { once = true;
        IOLog("MT2BTReader: [diag] first incomingData hit len=%u b0=0x%02x decode=%d\n",
              length, length ? b[0] : 0, drc); } }
    if (drc != 0) return;   /* not a recognized multitouch frame — drop */

    /* STEADY: first real frame → set fStreaming on self (target = the reader) so the
     * re-enable timer stops retrying 0xF1. Also mirrors bt_interpose_shim's gSteadyConn gate. */
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)target;
    if (self && !self->fStreaming) {
        self->fStreaming = true;
        if (gSteadyConn != gConnId) { gSteadyConn = gConnId;
            bt_conntrace(CSM_STEADY, CSM_EV_FRAME, channel, 0, 0, 0); }
    }

    /* Shared diag: per-frame edge coords (debug.mavericks_log>=2). want_first=false: CSM_STEADY above. */
    mavericks_diag_frame(MAVERICKS_DIAG_BT, &tf, /*want_first=*/false);

    /* Cross the seam as a VoodooInput satellite: translate to a wire event and message the mux
     * (found lazily — it attaches async after registerService; pre-attach frames drop). The mux
     * owns terminal conditioning + the fabricated AMD; gBtMux bridges the battery poll to it. */
    if (self) {
        if (!self->fMux) {
            OSIterator *it = self->getClientIterator();
            if (it) {
                OSObject *o;
                while ((o = it->getNextObject())) {
                    IOService *c = OSDynamicCast(IOService, o);
                    if (c && c->getProperty(VOODOO_INPUT_IDENTIFIER)) { self->fMux = c; break; }
                }
                it->release();
            }
            if (self->fMux) gBtMux = OSDynamicCast(com_schmonz_MavericksVoodooInput, self->fMux);
        }
        if (self->fMux) {
            VoodooInputEvent ev = mavericks_voodoo_from_frame(&tf, MT2_SPAN_X, MT2_SPAN_Y);
            self->messageClient(kIOMessageVoodooInputMessage, self->fMux, &ev, sizeof ev);
        }
    }
}

/* Control-channel accept delegate — INERT. Its sole purpose is to be the listenAt callback that
 * "accepts" the control channel (with the following waitForChannelState(OPEN)), reproducing what the
 * genuine IOBluetoothHIDDriver does to provoke the device to open PSM 19. It derefs nothing (not even
 * target), so it is safe to be called on an already-freed reader — which the battery interpose's
 * restore can leave in place. Battery GET_REPORT(0x90) sniffing lives in bt_control_shim, not here. */
void com_schmonz_MT2BTReader::controlData(IOService * /*target*/, IOBluetoothL2CAPChannel * /*channel*/,
                                          unsigned short /*length*/, void * /*data*/) {
}

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    resetTransportState();

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;
    IOLog("MT2BTReader: [diag] start() PSM=%u chan->isInactive=%d\n",
          fChannel->getPSM(), fChannel->isInactive());

    /* The main flow, as a sequence of named steps: */
    if (!marshalSetupInGate()) return false;    /* bind channel + arm session, in-gate */
    if (fIsControl) {                           /* control reader: owns the 0xF1 enable + battery poll */
        armInterposeTimer();                    /* deferred 0xF1 enable + battery poll once streaming */
    }

    IOLog("MT2BTReader: bound L2CAP channel, enabled multitouch (0xF1), listening\n");
    registerService();
    return true;
}

/* Step: zero the per-connection fields (extracted from start()). */
void com_schmonz_MT2BTReader::resetTransportState() {
    fIsControl = false;
    fInterposeTimer = 0;
    fInterposeTries = 0;
    fReEnableCount = 0;
    fStreaming = false;
    fMux = 0;
}

/* Step: run setupInGate on the channel's command gate. IOBluetoothFamily REQUIREs every
 * IOBluetoothObject call to run inside its workloop gate (calling sendTo/listenAt from this start()
 * thread panics "NOT called in IOWorkLoop"). Returns false if the channel has no gate. */
bool com_schmonz_MT2BTReader::marshalSetupInGate() {
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
    return true;
}

/* Step: arm the async timer for the deferred 0xF1 enable + battery poll. The enable is deferred
 * because firing it before the channel is OPEN flaps the link (~14s block; §B1-drive).
 * Invariant: fInterposeTimer != 0 => it is attached to the workloop. */
void com_schmonz_MT2BTReader::armInterposeTimer() {
    fInterposeTries = 0;
    IOWorkLoop *wl = getWorkLoop();
    if (wl) {
        fInterposeTimer = IOTimerEventSource::timerEventSource(
            this, &com_schmonz_MT2BTReader::interposeTimerFired);
        if (fInterposeTimer) {
            if (wl->addEventSource(fInterposeTimer) == kIOReturnSuccess)
                fInterposeTimer->setTimeoutMS(100);
            else { fInterposeTimer->release(); fInterposeTimer = 0; }
        }
    }
}

/* In-gate: null our listenAt callback. newDataIn bails when the callback (channel+0x110)
 * is NULL before it derefs the target (channel+0x118), so this makes it safe for the
 * channel to outlive us. Same-target listenAt(self, NULL) is accepted by the family. */
IOReturn com_schmonz_MT2BTReader::teardownInGate(OSObject * /*owner*/, void *arg0,
                                                 void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    /* Clear our listenAt delegate before we can be freed, else the channel's newDataIn derefs a
     * dangling reader (UAF panic). BOTH channels now register a delegate (interrupt = incomingData,
     * control = controlData), so clear whichever channel this reader owns. */
    if (self && self->fChannel) self->fChannel->listenAt(self, 0);
    return kIOReturnSuccess;
}

/* In the CONTROL channel's gate: save the control-channel delegate and swap in bt_control_shim,
 * so we can sniff GET_REPORT(0x90) responses. Same save-and-swap as the (removed) interrupt
 * interpose but on separate state (gBtControlState). arg0 = the control channel.
 * Returns NotReady if the slot is not set (no prior delegate; Task 3 addresses this). */
IOReturn com_schmonz_MT2BTReader::controlInterposeInGate(OSObject * /*owner*/, void *arg0,
                                                         void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    int rc = mavericks_splice_install(&kBtControlRow, ch, &mavericks_splice_kext_ops, &gBtControlState);
    if (rc == MAVERICKS_SPLICE_NOT_READY) return kIOReturnNotReady;
    if (rc != MAVERICKS_SPLICE_OK) return kIOReturnError;
    gCtrlInterposedChannel = ch;
    IOLog("MT2BTReader: battery control interpose installed (origCb=%p origTgt=%p)\n",
          gBtControlState.saved_cb, gBtControlState.saved_target);
    return kIOReturnSuccess;
}

/* In-gate: restore the original control-channel callback before we tear down. arg0 = channel. */
IOReturn com_schmonz_MT2BTReader::controlRestoreInGate(OSObject * /*owner*/, void *arg0,
                                                       void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    mavericks_splice_restore(ch, &kBtControlRow, &mavericks_splice_kext_ops, &gBtControlState);
    return kIOReturnSuccess;
}

/* In the CONTROL channel's gate: poll the battery. HIDP GET_REPORT(Input, report 0x90) = the 2-byte
 * request { 0x41, 0x90 } on the control channel (mirrors reEnableInGate's SET_REPORT enable). The
 * device answers with [0xA1][0x90][flags][cap] on the same channel, which bt_control_shim catches.
 * arg0 = the control reader; its fChannel is the PSM-17 control channel. */
IOReturn com_schmonz_MT2BTReader::pollBatteryInGate(OSObject * /*owner*/, void *arg0,
                                                    void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;
    static const uint8_t kGetBattery[] = { MAVERICKS_HIDP_GET_REPORT_INPUT, MAVERICKS_BATTERY_REPORT_ID };
    self->fChannel->sendTo((void *)kGetBattery, sizeof(kGetBattery), 0, self, 0, 0);
    return kIOReturnSuccess;
}

/* In the CONTROL channel's command gate: re-send the MT2's 0xF1 multitouch enable (same
 * HIDP SET_REPORT(feature) payload setupInGate sends). Without BNB the device can remain in
 * mouse mode (report 0x02) after our initial enable, so this forces it back to multitouch
 * (report 0x31). arg0 = the control reader; its fChannel is the PSM-17 control channel. */
IOReturn com_schmonz_MT2BTReader::reEnableInGate(OSObject * /*owner*/, void *arg0,
                                                 void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)arg0;
    if (!self || !self->fChannel) return kIOReturnNoDevice;
    static const uint8_t kEnable[] = { MAVERICKS_HIDP_SET_REPORT_FEATURE, MAVERICKS_ENABLE_REPORT_ID, 0x02, 0x01 };
    self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
    return kIOReturnSuccess;
}

/* interposeTimerFired: send the 0xF1 multitouch enable on the control channel, retrying until
 * incomingData delivers the first real frame (gSteadyConn==gConnId / fStreaming). Once streaming,
 * switch to battery polling. The deferred enable is still required: sendTo before the channel is
 * OPEN blocks ~14s and tears the link (the B1-drive root cause, 2026-06-21; §B1-drive). */
void com_schmonz_MT2BTReader::interposeTimerFired(OSObject *owner, IOTimerEventSource *ts) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)owner;

    IOCommandGate *cg = self->fChannel
        ? ((IOBluetoothObject *)self->fChannel)->getCommandGate() : 0;

    if (gSteadyConn != gConnId) {
        /* Still waiting for first frame: re-send 0xF1. */
        if (cg) cg->runAction(&com_schmonz_MT2BTReader::reEnableInGate, self);
        self->fReEnableCount++;
        if (self->fReEnableCount == 8)
            IOLog("MT2BTReader: initial re-enable push done; retrying gently until first frame\n");
        ts->setTimeoutMS(self->fReEnableCount < 8 ? 250 : 1000);
        return;
    }

    /* First frame received — streaming confirmed. */
    if (self->fReEnableCount >= 0) {
        IOLog("MT2BTReader: multitouch confirmed (first frame after %d enables)\n", self->fReEnableCount);
        bt_conntrace(CSM_MT_MODE, CSM_EV_MT_MODE_CONFIRMED, self->fChannel, 0, 0, 0);
        self->fReEnableCount = -1;
    }

    /* Battery: install the CONTROL-channel shim (once) so we can catch GET_REPORT(0x90)
     * responses. Retry each tick until the slot is populated; idempotent once installed. */
    if (!gCtrlInterposedChannel && cg)
        cg->runAction(&com_schmonz_MT2BTReader::controlInterposeInGate, self->fChannel);

    if (cg) cg->runAction(&com_schmonz_MT2BTReader::pollBatteryInGate, self);
    ts->setTimeoutMS(MAVERICKS_BATTERY_POLL_MS);
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    bt_conntrace(CSM_IDLE, CSM_EV_DISCONNECT, fChannel, 0, 0, 0);
    /* If we're the interrupt reader that armed the session source, clear it so the
     * direct shim stops feeding through a freed instance. */
    if (gInterruptReader == this) gInterruptReader = 0;
    /* We are a VoodooInput satellite: as this provider stops, IOKit detaches + stops the mux
       (our client), which tears down its own terminal AMD. Just drop our borrowed references. */
    fMux = 0;
    if (gBtMux) gBtMux = 0;   /* forget the located mux (its backend owns the battery node + dedup) */
    /* Deregister our incoming-data callback in-gate BEFORE we can be freed, or the
     * channel's newDataIn will dereference a dangling pointer (use-after-free panic
     * seen when the installer unloaded the live kext while the trackpad streamed). */
    if (fChannel) {
        IOCommandGate *gate = fChannel->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::teardownInGate, this);
    }
    /* Cancel and remove the timer (order: cancel -> remove -> release, so an in-flight
     * handler always returns before we free it — IOWorkLoop::removeEventSource closes the
     * workloop gate, blocking until any in-flight handler returns). */
    if (fInterposeTimer) {
        fInterposeTimer->cancelTimeout();
        if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fInterposeTimer);
        fInterposeTimer->release();
        fInterposeTimer = 0;
    }
    /* Battery: restore the original control-channel delegate (separate save-and-swap), so the
     * about-to-be-freed bt_control_shim is never called. */
    if (gCtrlInterposedChannel) {
        IOCommandGate *gate = ((IOBluetoothObject *)gCtrlInterposedChannel)->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::controlRestoreInGate, gCtrlInterposedChannel);
        gCtrlInterposedChannel = 0;
    }
    /* No terminal AMD to tear down here anymore — the mux owns it and cleans it up when it
     * detaches as our client (above). */
    fChannel = 0;
    IOService::stop(provider);
}
