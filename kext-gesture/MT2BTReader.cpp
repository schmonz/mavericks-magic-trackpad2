/* MT2BTReader — the Bluetooth transport reader for the Magic Trackpad 2.
 *
 * Two halves. PER-TRANSPORT (the ~3%): bind the L2CAP channels, decode each raw MT2 0x31
 * frame (mt2_bt_decode), and declare BT config — BNB props, sensor geometry, the 0xF1
 * enable. SHARED ENGINE (the ~97%) is what it feeds. There is ONE seam: incomingData
 * decodes to a mt2_frame and hands it to gActiveMT2Gesture->submitFrame; the shared
 * session conditions it, mt1_encode's report 0x28, and drives the fabricated AMD directly.
 * No BNBTrackpadDevice is ever started. No decision logic lives in this file.
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
#include "MT2Gesture.h"
#include "amd_shim.h"          /* AppleMultitouchDevice handleTouchFrame / handlePointerEventFromDevice */
#include "mt1_encode.h"

/* Compiled as C++ under the kext toolchain (so is mt2_bt_decode.c), so these resolve
 * with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt2_battery.h"    /* mt2_parse_battery_report — shared pure decode of report 0x90 */
#include "mt2_pipeline.h"   /* MT2_EVENT_DRIVEN */
#include "mt2_log.h"           /* MT2_DLOG (runtime debug.mt2_log) */
#include "mt2_diag.h"          /* shared per-transport stream diagnostics (report id / first frame / edge / gap) */
#include "../src/conn_trace.h" /* CONNTRACE emitter (connect-flap measurement) */
#include "../src/mt2_stack.h"  /* canonical RE facts: vtable slots, field offsets, props */
#include "../src/mt2_coordinator.h"  /* transport-coordinator seam (no-op for MT2) */
#include "mt2_synth_amd.h"           /* mt2_synth_amd_build/amd/teardown — multi-instance fabricated AMD */
/* mt2_splice_kext.h -> mt2_splice.h -> vtable_clone.h requires these macros before the include. */
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "mt2_splice_kext.h"   /* declarative interpose engine + kext ops (mt2_splice_kext_ops). */

/* Battery poll cadence (ms). The MT2 only answers a GET_REPORT(0x90) — it never streams battery —
 * so the control reader polls on this interval once the connection is settled. A battery moves
 * slowly; 30 s keeps the prefpane number fresh without churning the control channel. */
#define MT2_BATTERY_POLL_MS  30000

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

/* Field offsets: canonical values + re/ commands live in ../src/mt2_stack.h. These are readable
 * local aliases so the numbers exist in exactly one place (no doc/build drift). */
#define L2CAP_DELEGATE_CB_OFF       MT2_OFF_L2CAP_DELEGATE_CB     /* L2CAP delegate cb (+8 = target)*/

/* The fabricated AppleMultitouchDevice context for BT.
 * Non-NULL only when the connection is up and the build succeeded.
 * A single global (one device at a time) — cleared on stop after the session deregisters. */
static mt2_synth_amd_ctx *gBtAmdCtx = 0;

/* The PSM-19 (interrupt) reader instance — the session's active frame source. incomingData
 * feeds decoded frames through gActiveMT2Gesture->submitFrame(gInterruptReader, ...). */
static IOService *gInterruptReader = 0;

/* Control-delegate seam state (battery channel). gCtrlInterposedChannel is the "which channel"
 * tracker (where to run the restore). */
static mt2_splice_state_t gBtControlState;
static IOBluetoothL2CAPChannel *gCtrlInterposedChannel = 0;

static uint32_t bt_uptime_ms(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint32_t)(s * 1000 + u / 1000);
}

/* Encode clock for the BT feed: kernel uptime in ms — the SAME clock the sink used before it
 * moved here from MT2Gesture (clock_get_uptime, not clock_get_system_microtime), so the frame
 * timestamps are unchanged by the move. */
static uint32_t bt_encode_uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* BT transport sink (registered with the engine at connectionEstablished; calls arrive under
 * the session lock). Delivery target = the fabricated AMD (gBtAmdCtx). NULL-guards drop
 * deliveries during bring-up. */
static void *bt_sink_amd(void) { return (void *)mt2_synth_amd_amd(gBtAmdCtx); }
static void bt_sink_feed_frame(void *ctx, const mt2_frame *frame) {
    (void)ctx;
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)bt_sink_amd();
    if (!amd) return;
    /* EDGE-CLAMP PROBE (debug.mt2_log>=2): per-frame decoded contact-0 x/y at the encode point. */
    if (frame->contact_count > 0)
        MT2_DLOG(2, "feed x=%d y=%d -> btAMD", frame->transducers[0].currentCoordinates.x,
                 frame->transducers[0].currentCoordinates.y);
    uint8_t mt1[256];
    int n = mt1_encode(frame, mt1, sizeof(mt1), bt_encode_uptime_ms());
    if (n <= 0) return;
    amd->handleTouchFrame(mt1, (unsigned int)n);
}
static void bt_sink_post_button_edge(void *ctx, unsigned mask) {
    (void)ctx;
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)bt_sink_amd();
    if (!amd) return;
    MT2_DLOG(2, "post_button_edge mask=0x%x -> btAMD", mask);
    amd->handlePointerEventFromDevice(0, 0, mask, 0);
}
static IOReturn bt_sink_inject(void *ctx, const unsigned char *bytes, unsigned int len) {
    (void)ctx;
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)bt_sink_amd();
    if (!amd) return kIOReturnNotReady;
    return amd->handleTouchFrame((unsigned char *)bytes, len);
}
static const mt2_transport_sink_t kBtSink =
    { bt_sink_feed_frame, bt_sink_post_button_edge, bt_sink_inject, 0 };

/* Connect-flap measurement: each connection attempt gets an id (bumped on control-channel open); we
 * emit one canonical CONNTRACE line per transition through the SHARED conn_trace_format() (kernel emit
 * and `re/conn-trace` parsing can't drift). Gated at debug.mt2_log>=1. */
static int gConnId = 0;
/* The connId whose FIRST real multitouch frame we've seen (STEADY). Set by incomingData; read by
 * interposeTimerFired to gate the enable RETRY — re-enable ONLY while gSteadyConn != gConnId. This is
 * the reconnect fix's whole teardown-safety; see explanation.md "Reconnect re-enable ... teardown-safe". */
static volatile int gSteadyConn = -1;
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

/* Battery bridge: publish the report-0x90 capacity on the fabricated AMD node.
 * Task 3 re-homes this to the fabricated AMD's node; for now it is a no-op
 * (gGenuineBnb is gone — Task 3 wires the fabricated node instead). */
static int        gLastBattPct = -1;
static IOService *gLastBattBnb = 0;
static void mt2_publish_battery(IOService *node, uint8_t pct) {
    /* debug.mt2_batt override: force a value for prefpane UI testing (e.g. 0 to exercise the pane's
     * low-battery / Change-Batteries painting). -1 = off (use the real device value). */
    if (gMT2BattOverride >= 0 && gMT2BattOverride <= 100) pct = (uint8_t)gMT2BattOverride;
    if (pct > 100) return;                     /* capacity is 0-100; ignore out-of-range */
    if (node == gLastBattBnb && (int)pct == gLastBattPct) return;   /* same node + same value */
    gLastBattPct = (int)pct; gLastBattBnb = node;
    OSNumber *n = OSNumber::withNumber((unsigned long long)pct, 32);
    if (n) { node->setProperty("BatteryPercent", n); n->release(); }
    MT2_DLOG(1, "battery = %u%% -> published BatteryPercent", (unsigned)pct);
}

/* If `data`/`len` is a battery report (0x90, optional 0xA1 transport byte), publish the capacity as
 * "BatteryPercent" on the FABRICATED AMD node (the BNB node is gone). mt2_synth_amd_amd() returns NULL
 * until the AMD is built + ready and again once teardown starts, so this self-fences. The pure parse is
 * host-tested (tests/test_battery.c). No-op when the packet isn't 0x90 or no AMD is up. */
static void mt2_maybe_publish_battery(const void *data, size_t len) {
    uint8_t pct;
    if (!mt2_parse_battery_report((const uint8_t *)data, len, &pct)) return;
    AppleMultitouchDevice *amd = mt2_synth_amd_amd(gBtAmdCtx);
    if (amd) mt2_publish_battery((IOService *)amd, pct);
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
    if (length > 0) mt2_diag_saw_id(MT2_DIAG_BT, (length > 1 && b[0] == 0xA1) ? b[1] : b[0]);
    mt2_maybe_publish_battery(data, length);
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
static const mt2_splice_row_t kBtControlRow = {
    "bt-control", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0, 0,
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
        /* Build the SP1-hardened fabricated AMD: we own the interrupt channel's
         * delegate directly (listenAt below); no BNB is started, no interpose needed. */
        gBtAmdCtx = mt2_synth_amd_build(gActiveMT2Gesture);
        if (!gBtAmdCtx) IOLog("MT2BTReader: fabricated AMD build FAILED - no cursor\n");
        /* Register OUR delegate on the interrupt channel: incomingData decodes 0x31 -> submitFrame.
         * listenAt is safe here because no BNB races us for this channel (no manual-start). */
        self->fChannel->listenAt(self, &com_schmonz_MT2BTReader::incomingData);
        if (gActiveMT2Gesture) {
            IOLog("MT2BTReader: DIRECT terminal — fabricated AMD, direct L2CAP listener\n");
            gActiveMT2Gesture->connectionEstablished(self, MT2_EVENT_DRIVEN,
                                                     &mt2_policy_default, &kBtSink);
        } else
            IOLog("MT2BTReader: ENGINE NOT PUBLISHED at interrupt bind — input will be dead until reconnect (registration race)\n");
    }

    self->fIsControl = (psm == 0x11);
    /* The 0xF1 multitouch enable is DEFERRED to the phase-2 reEnable timer (firing it before the
     * channel is OPEN flaps the link) — see explanation.md "Deferred 0xF1 multitouch enable". */
    IOLog("MT2BTReader: setup on PSM=%u\n", psm);
    /* A control channel coming up starts a new connection attempt — bump the id, mark CONTROL_UP, and
     * reset the shared diag so a reconnect re-observes report ids + re-arms the first-frame/gap markers. */
    if (psm == 0x11) { gConnId++; mt2_diag_reset(MT2_DIAG_BT); bt_conntrace(CSM_CONTROL_UP, CSM_EV_CONTROL_OPEN, self->fChannel, 0, 0, 0); }
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
    if (rlen > 0) mt2_diag_raw(MT2_DIAG_BT, report[0]);

    mt2_frame tf;
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

    /* Shared diag: per-frame edge coords (debug.mt2_log>=2). want_first=false: CSM_STEADY above. */
    mt2_diag_frame(MT2_DIAG_BT, &tf, /*want_first=*/false);

    /* Cross the seam: hand the decoded frame to the SHARED session via the fabricated AMD sink. */
    if (gActiveMT2Gesture && gInterruptReader)
        gActiveMT2Gesture->submitFrame(target, &tf);   /* target = this reader */
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
    /* Clear our delegate only on the interrupt channel (PSM 19) where we set it.
     * The control reader never set a delegate via listenAt, so clearing there is a no-op. */
    if (self && !self->fIsControl && self->fChannel) self->fChannel->listenAt(self, 0);
    return kIOReturnSuccess;
}

/* In the CONTROL channel's gate: save the control-channel delegate and swap in bt_control_shim,
 * so we can sniff GET_REPORT(0x90) responses. Same save-and-swap as the (removed) interrupt
 * interpose but on separate state (gBtControlState). arg0 = the control channel.
 * Returns NotReady if the slot is not set (no prior delegate; Task 3 addresses this). */
IOReturn com_schmonz_MT2BTReader::controlInterposeInGate(OSObject * /*owner*/, void *arg0,
                                                         void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    int rc = mt2_splice_install(&kBtControlRow, ch, &mt2_splice_kext_ops, &gBtControlState);
    if (rc == MT2_SPLICE_NOT_READY) return kIOReturnNotReady;
    if (rc != MT2_SPLICE_OK) return kIOReturnError;
    gCtrlInterposedChannel = ch;
    IOLog("MT2BTReader: battery control interpose installed (origCb=%p origTgt=%p)\n",
          gBtControlState.saved_cb, gBtControlState.saved_target);
    return kIOReturnSuccess;
}

/* In-gate: restore the original control-channel callback before we tear down. arg0 = channel. */
IOReturn com_schmonz_MT2BTReader::controlRestoreInGate(OSObject * /*owner*/, void *arg0,
                                                       void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    mt2_splice_restore(ch, &kBtControlRow, &mt2_splice_kext_ops, &gBtControlState);
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
    static const uint8_t kGetBattery[] = { MT2_HIDP_GET_REPORT_INPUT, MT2_BATTERY_REPORT_ID };
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
    static const uint8_t kEnable[] = { MT2_HIDP_SET_REPORT_FEATURE, MT2_ENABLE_REPORT_ID, 0x02, 0x01 };
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
    ts->setTimeoutMS(MT2_BATTERY_POLL_MS);
}

void com_schmonz_MT2BTReader::stop(IOService *provider) {
    bt_conntrace(CSM_IDLE, CSM_EV_DISCONNECT, fChannel, 0, 0, 0);
    /* If we're the interrupt reader that armed the session source, clear it so the
     * direct shim stops feeding through a freed instance. */
    if (gInterruptReader == this) gInterruptReader = 0;
    /* Deregister from the engine: after this returns no session effect can reach our sink.
       No-op unless we are the active source (the interrupt reader) — the control reader's
       stop leaves the session alone. */
    if (gActiveMT2Gesture) gActiveMT2Gesture->connectionClosed(this);
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
    /* Tear down the fabricated AMD we built in setupInGate.
     * NULL gBtAmdCtx first so an in-flight watchdog flush sees no AMD before we quiesce,
     * then free after the drain. */
    if (gBtAmdCtx) {
        mt2_synth_amd_ctx *btAmdToTear = gBtAmdCtx;
        gBtAmdCtx = 0;
        gLastBattBnb = 0;  /* forget the torn-down node so a reused address can't false-match */
        if (gActiveMT2Gesture) gActiveMT2Gesture->quiesceDelivery();
        mt2_synth_amd_teardown(gActiveMT2Gesture, btAmdToTear);
        IOLog("MT2BTReader: fabricated AMD torn down\n");
    }
    fChannel = 0;
    IOService::stop(provider);
}
