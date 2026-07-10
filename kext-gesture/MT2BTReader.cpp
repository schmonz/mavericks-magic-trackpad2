/* MT2BTReader — the Bluetooth transport reader for the Magic Trackpad 2.
 *
 * Two halves. PER-TRANSPORT (the ~3%): bind the L2CAP channels, decode each raw MT2 0x31
 * frame (mt2_bt_decode), and declare BT config — BNB props, sensor geometry, the 0xF1
 * enable. SHARED ENGINE (the ~97%) is what it feeds. There is ONE seam: bt_interpose_shim
 * decodes to a VoodooInputEvent and hands it to gActiveMT2Gesture->submitFrame; the shared
 * session conditions it, mt1_encode's report 0x28, and drives Apple's own genuine
 * BNBTrackpadDevice (manual-started here). No decision logic lives in this file.
 *
 * Dirty tricks, named where used: bt_interpose_shim (L2CAP delegate-callback splice),
 * installBnbGeometry (instance-scoped vtable clone), the deferred 0xF1 multitouch enable.
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
#include "gh_default_adapter.h"      /* shared generic alloc/class_ok/start/detach/terminate/release */

extern "C" {
#include "mt2_geometry.h"      /* single source of the sensor-geometry D-report payloads */
}
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "vtable_clone.h"      /* instance-scoped vtable clone/override/restore */
#include "mt2_splice_kext.h"   /* declarative interpose engine + kext ops (mt2_splice_kext_ops).
                                * After vtable_clone.h so its VTC_ALLOC/FREE guard is satisfied. */

/* Vtable-slot INDICES for the geometry override. The byte offsets are the canonical facts in
 * ../src/mt2_stack.h (the single build-consumed source; see docs/mt-stack/); these are thin local
 * aliases that convert byte-offset -> slot index. The query probes the LENGTH (0xcd8) FIRST and
 * only then the DATA (0xcc8), so we override both (installBnbGeometry). */
#define GMR_SLOT_INDEX     (MT2_VT_getMultitouchReport     / sizeof(void *))  /* DATA fetch         */
#define GMRINFO_SLOT_INDEX (MT2_VT_getMultitouchReportInfo / sizeof(void *))  /* LENGTH probe (1st) */
/* Clone span: must cover the highest dispatched slot (>= 0xd78) + margin. Generous fixed span. */
#define BNB_VTABLE_SPAN  0x2000

/* Battery poll cadence (ms). The MT2 only answers a GET_REPORT(0x90) — it never streams battery —
 * so the control reader polls on this interval once the connection is settled. A battery moves
 * slowly; 30 s keeps the prefpane number fresh without churning the control channel. */
#define MT2_BATTERY_POLL_MS  30000

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

OSDefineMetaClassAndStructors(com_schmonz_MT2BTReader, IOService)

/* Path A architecture: manual-start a genuine BNBTrackpadDevice on the control channel, YIELD the
 * interrupt-channel delegate to BNB (its listenAt must succeed or §S2.5 panics), then splice our
 * MT2->MT1 shim onto that channel's delegate-callback slot. Apple's BNB drives cursor/gestures/
 * prefpane; we condition + inject. See explanation.md "End-to-end data flow (full-BNB)". */

/* Field offsets: canonical values + re/ commands live in ../src/mt2_stack.h. These are readable
 * local aliases so the numbers exist in exactly one place (no doc/build drift). */
#define BNB_HANDLER_OFF             MT2_OFF_BNB_AMD               /* AMD* (AppleMultitouchDevice*)  */
#define L2CAP_DELEGATE_CB_OFF       MT2_OFF_L2CAP_DELEGATE_CB     /* L2CAP delegate cb (+8 = target)*/
#define BNB_INTERRUPT_CHANNEL_OFF   MT2_OFF_BNB_INTERRUPT_CHANNEL /* BNBDevice::_interruptChannel    */

/* The genuine BNBTrackpadDevice the control reader manual-starts (Path A). The interpose installer
 * reads it (via +0xf0) to find BNB's interrupt channel. Single device → one global, like
 * gActiveMT2Gesture. NULL when no genuine device is up. */
IOService *gGenuineBnb = 0;

/* The PSM-19 (interrupt) reader instance — the session's active frame source. bt_interpose_shim
 * feeds decoded frames through gActiveMT2Gesture->submitFrame(gInterruptReader, ...). */
static IOService *gInterruptReader = 0;

/* Interrupt-delegate seam state (mt2_splice, MEM_SLOT). gInterposedChannel stays — it is the
 * "which channel" tracker (where to run the restore), the caller's concern, not the engine's. */
static mt2_splice_state_t gBtInterruptState;
static IOBluetoothL2CAPChannel *gInterposedChannel = 0;

/* Control-delegate seam state (battery channel). Separate state from the interrupt seam so the
 * interrupt shim's saved cb is never clobbered. gCtrlInterposedChannel stays (the tracker). */
static mt2_splice_state_t gBtControlState;
static IOBluetoothL2CAPChannel *gCtrlInterposedChannel = 0;

/* BNB geometry seam state (mt2_splice). The row is declared after the geometry shims it names
 * (below); installBnbGeometry/removeBnbGeometry stay the call sites, timing unchanged. */
static mt2_splice_state_t gBnbSpliceState;

/* getMultitouchReport override on OUR transport instance (vtable slot 0xcc8). ABI:
 *   getMultitouchReport(transport, uchar reportId, uchar* buf, uint* len, int count).
 * Answer the geometry D-reports from the shared source, everything else unsupported — so BNB's AMD
 * bring-up (cacheDeviceProperties) publishes real geometry and the MTDevice is born with right dims. */
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
 * the report LENGTH via this BEFORE the data fetch, so the stock unsupported stub short-circuits the
 * whole query before getMultitouchReport (0xcc8) is reached — we must answer LENGTH here too. Same
 * signature as getMultitouchReport (per ABM staticReportInfoHandler @0x1356). */
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

/* BNB geometry seam as a declarative row: 2-slot vtable clone (data 0xcc8 + info 0xcd8),
 * class-gated to the genuine transport. The CLASS_NAME gate reproduces installBnbGeometry's
 * abort-unless-BNBTrackpadDevice/BluetoothMultitouchTransport check. */
static const mt2_splice_row_t kBnbGeometryRow = {
    "bnb-geometry", MT2_SPLICE_VTABLE_CLONE, MT2_GATE_CLASS_NAME,
    "BNBTrackpadDevice", "BluetoothMultitouchTransport",
    GMR_SLOT_INDEX, (void *)&mt2_bnb_get_multitouch_report,
    GMRINFO_SLOT_INDEX, (void *)&mt2_bnb_get_multitouch_report_info, BNB_VTABLE_SPAN
};

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
 * the session lock). Delivery target = the AMD the genuine BNB spawned, read LIVE from
 * gGenuineBnb+0x1b0 on every call — a reconnect swaps the node under us, and the old per-frame
 * setBnbTarget refresh did exactly this read. NULL-guards drop deliveries during bring-up. */
static void *bt_bnb_amd(void) {
    return gGenuineBnb ? *(void **)((uint8_t *)gGenuineBnb + BNB_HANDLER_OFF) : 0;
}
static void bt_sink_feed_frame(void *ctx, const VoodooInputEvent *frame) {
    (void)ctx;
    void *amd = bt_bnb_amd();
    if (!amd) return;
    /* EDGE-CLAMP PROBE (debug.mt2_log>=2): per-frame decoded contact-0 x/y at the encode point. */
    if (frame->contact_count > 0)
        MT2_DLOG(2, "feed x=%d y=%d -> bnbAMD", frame->transducers[0].currentCoordinates.x,
                 frame->transducers[0].currentCoordinates.y);
    uint8_t mt1[256];
    int n = mt1_encode(frame, mt1, sizeof(mt1), bt_encode_uptime_ms());
    if (n <= 0) return;
    ((AppleMultitouchDevice *)amd)->handleTouchFrame(mt1, (unsigned int)n);
}
static void bt_sink_post_button_edge(void *ctx, unsigned mask) {
    (void)ctx;
    void *amd = bt_bnb_amd();
    if (!amd) return;
    MT2_DLOG(2, "post_button_edge mask=0x%x -> bnbAMD", mask);
    ((AppleMultitouchDevice *)amd)->handlePointerEventFromDevice(0, 0, mask, 0);
}
static IOReturn bt_sink_inject(void *ctx, const unsigned char *bytes, unsigned int len) {
    (void)ctx;
    void *amd = bt_bnb_amd();
    if (!amd) return kIOReturnNotReady;
    return ((AppleMultitouchDevice *)amd)->handleTouchFrame((unsigned char *)bytes, len);
}
static const mt2_transport_sink_t kBtSink =
    { bt_sink_feed_frame, bt_sink_post_button_edge, bt_sink_inject, 0 };

/* Connect-flap measurement: each connection attempt gets an id (bumped on control-channel open); we
 * emit one canonical CONNTRACE line per transition through the SHARED conn_trace_format() (kernel emit
 * and `re/conn-trace` parsing can't drift). Gated at debug.mt2_log>=1. */
static int gConnId = 0;
/* The connId whose FIRST real multitouch frame we've seen (STEADY). Set by bt_interpose_shim; read by
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

/* Battery bridge: publish the report-0x90 capacity as the "BatteryPercent" OSNumber on the genuine
 * BNB node — exactly the property the Trackpad pane reads. setProperty is registry-lock-guarded, safe
 * from this newDataIn context. Dedup is keyed on BOTH value AND node instance (each reconnect makes a
 * fresh node; value-only dedup missed the republish → pane read -1.0). gLastBattBnb is reset in
 * start()/stop() so a malloc-reused address can't false-match. See explanation.md "Battery: publish on
 * the BNB node + the ExtendedFeatures presence gate". */
static int        gLastBattPct = -1;
static IOService *gLastBattBnb = 0;
static void mt2_publish_battery(IOService *bnb, uint8_t pct) {
    /* debug.mt2_batt override: force a value for prefpane UI testing (e.g. 0 to exercise the pane's
     * low-battery / Change-Batteries painting). -1 = off (use the real device value). */
    if (gMT2BattOverride >= 0 && gMT2BattOverride <= 100) pct = (uint8_t)gMT2BattOverride;
    if (pct > 100) return;                     /* capacity is 0-100; ignore out-of-range */
    if (bnb == gLastBattBnb && (int)pct == gLastBattPct) return;   /* same node + same value */
    gLastBattPct = (int)pct; gLastBattBnb = bnb;
    OSNumber *n = OSNumber::withNumber((unsigned long long)pct, 32);
    if (n) { bnb->setProperty("BatteryPercent", n); n->release(); }
    MT2_DLOG(1, "battery = %u%% -> published BatteryPercent", (unsigned)pct);
}

/* If `data`/`len` is a battery report (0x90, optional 0xA1 transport byte), publish its capacity
 * on the genuine BNB node. Shared by both channel shims; the pure parse is host-tested
 * (tests/test_battery.c). No-op when there is no genuine node or the packet isn't 0x90. */
static void mt2_maybe_publish_battery(const void *data, size_t len) {
    if (!gGenuineBnb) return;
    uint8_t pct;
    if (mt2_parse_battery_report((const uint8_t *)data, len, &pct))
        mt2_publish_battery(gGenuineBnb, pct);
}

/* THE SEAM (dirty trick #1: L2CAP delegate-callback splice). This runs in place of BNB's own data
 * callback on the interrupt channel (interposeInGate swaps it in). It decodes the raw MT2 0x31 report
 * to a VoodooInputEvent and hands that ONE object across to the shared engine
 * (gActiveMT2Gesture->submitFrame); everything downstream (condition → mt1_encode → drive Apple's AMD)
 * is shared. Runs in the channel's BT workloop (newDataIn context), same as Apple's own. */
static void bt_interpose_shim(IOService *target, IOBluetoothL2CAPChannel *channel,
                              unsigned short length, void *data) {
    (void)target;
    const uint8_t *b = (const uint8_t *)data;
    const uint8_t *rep = b;
    size_t rlen = length;
    if (length > 0 && b[0] == 0xA1) { rep = b + 1; rlen = length - 1; }   /* strip transport byte */

    /* Shared diag: distinct report id (once) + resume-after-gap. Also reveals whether battery report
     * 0x90 ever arrives on the interrupt channel vs needing a control-channel poll. */
    if (rlen > 0) mt2_diag_raw(MT2_DIAG_BT, rep[0]);
    mt2_maybe_publish_battery(rep, rlen);

    VoodooInputEvent tf;
    int drc = mt2_bt_decode(rep, rlen, &tf);
    { static bool once = false; if (!once) { once = true;
        IOLog("MT2BTReader: [diag] first shim hit len=%u b0=0x%02x decode=%d\n",
              length, length ? b[0] : 0, drc); } }
    if (drc != 0) return;                                                 /* not multitouch — drop */

    /* STEADY: first real frame of this connection = the pad streams end-to-end. Recording gSteadyConn
     * here STOPS the enable-retry loop in interposeTimerFired (teardown-safety; see gSteadyConn decl). */
    if (gSteadyConn != gConnId) { gSteadyConn = gConnId;
        bt_conntrace(CSM_STEADY, CSM_EV_FRAME, channel, gGenuineBnb, 0, 0); }

    /* Shared diag: per-frame edge coords (debug.mt2_log>=2) — decoded contact-0 X/Y, to catch a
     * downstream saturation band (re/mt-contacts). want_first=false: BT's first-frame signal is the
     * FUNCTIONAL CSM_STEADY above, not a duplicate observational marker. */
    mt2_diag_frame(MT2_DIAG_BT, &tf, /*want_first=*/false);

    /* Cross the seam: hand the decoded frame to the SHARED session; the registered BT sink
       (kBtSink) encodes and delivers to BNB's own spawned AMD. */
    if (gActiveMT2Gesture && gInterruptReader)
        gActiveMT2Gesture->submitFrame(gInterruptReader, &tf);
}

/* Delegate-callback ABI: IOBluetoothL2CAPChannel::newDataIn invokes (channel+0x110) as
 * cb(target, channel, length, data), target read from channel+0x118. */
typedef void (*bt_l2cap_cb_t)(IOService *, IOBluetoothL2CAPChannel *, unsigned short, void *);

/* CONTROL-channel (PSM 17) delegate shim. Unlike the interrupt shim (which consumes touch frames),
 * this is peek-and-forward: BNB owns the control plane, so EVERY PDU must reach BNB's original
 * callback unchanged. We only sniff GET_REPORT(0x90) responses ([0xA1][0x90][flags][cap]) to publish
 * the battery %. Runs in the control channel's BT workloop (newDataIn context). */
static void bt_control_shim(IOService *target, IOBluetoothL2CAPChannel *channel,
                            unsigned short length, void *data) {
    (void)target;
    const uint8_t *b = (const uint8_t *)data;
    /* Log distinct ids seen on control (confirmed the battery 0x90 response arrives here, not on the
     * interrupt channel). Strip the 0xA1 transport byte for the id, matching the interrupt diag.
     * saw_id (not _raw): control-plane battery polls must NOT reset the touch stream's idle-gap clock. */
    if (length > 0) mt2_diag_saw_id(MT2_DIAG_BT, (length > 1 && b[0] == 0xA1) ? b[1] : b[0]);
    mt2_maybe_publish_battery(data, length);
    /* Forward to BNB's real delegate — the control plane depends on seeing every PDU. */
    if (gBtControlState.saved_cb)
        ((bt_l2cap_cb_t)gBtControlState.saved_cb)(
            (IOService *)gBtControlState.saved_target, channel, length, data);
}

/* The two BT L2CAP delegate seams as declarative rows (MEM_SLOT: swap the cb at +0x110, the
 * engine saves both cb and the adjacent +0x118 target). SLOT_POPULATED gate = the old
 * "not set yet / already ours -> NotReady" precondition. */
static const mt2_splice_row_t kBtInterruptRow = {
    "bt-interrupt", MT2_SPLICE_MEM_SLOT, MT2_GATE_SLOT_POPULATED, 0, 0,
    L2CAP_DELEGATE_CB_OFF, (void *)&bt_interpose_shim, 0, 0, 0
};
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
    if (psm == 0x13) {
        gInterruptReader = self;   /* the session source; the Path A hybrid shim feeds frames as this */
        bt_conntrace(CSM_INTERRUPT_BOUND, CSM_EV_INTERRUPT_PUBLISHED, self->fChannel, 0, 0, 0);
        if (gActiveMT2Gesture)
            gActiveMT2Gesture->connectionEstablished(self, MT2_EVENT_DRIVEN,
                                                     &mt2_policy_default, &kBtSink);
        else
            IOLog("MT2BTReader: ENGINE NOT PUBLISHED at interrupt bind — input will be dead until reconnect (registration race)\n");
    }

    /* PSM 19 (interrupt): genuine BNB owns this channel's delegate; we do NOT listen on it ourselves
     * (holding it makes BNB's listenAt return 0xe00002bc → forced teardown → panic, §S2.5/§S2.6). We
     * interpose BNB's delegate later (interposeInGate) instead of owning the channel. */
    self->fIsControl = (psm == 0x11);
    /* The 0xF1 multitouch enable is DEFERRED to the phase-2 reEnable timer (firing it before the
     * channel is OPEN flaps the link) — see explanation.md "Deferred 0xF1 multitouch enable". */
    IOLog("MT2BTReader: setup on PSM=%u\n", psm);
    /* A control channel coming up starts a new connection attempt — bump the id, mark CONTROL_UP, and
     * reset the shared diag so a reconnect re-observes report ids + re-arms the first-frame/gap markers. */
    if (psm == 0x11) { gConnId++; mt2_diag_reset(MT2_DIAG_BT); bt_conntrace(CSM_CONTROL_UP, CSM_EV_CONTROL_OPEN, self->fChannel, 0, 0, 0); }
    return kIOReturnSuccess;
}

/* Build the OSDictionary passed to BNBTrackpadDevice::init. multitouchProperties() reads
 * "DefaultMultitouchProperties", so the table must carry the genuine config (parser-type 1000 + the
 * MultitouchHID plugin) so the AMD BNB spawns is recognized by MultitouchSupport and drives the cursor.
 * Mirrors the BNBTrackpadDriver personality. Some seeds are GENUINE per-transport and must NOT be
 * unified with usb_build_init_props: parser-options (BT's own 0x2F) and the DefaultMultitouchProperties
 * nesting (USB is flat); geometry is answered dynamically here (getMultitouchReport), not seeded — see
 * mt2_stack.h + decisions.md "genuine-reuse tax". Returns the dict as void* (the cfg.build_props
 * signature), NULL on alloc failure. */
static void *bt_build_bnb_props(void) {
    OSDictionary *top    = OSDictionary::withCapacity(1);
    OSDictionary *mt     = OSDictionary::withCapacity(9);
    OSDictionary *plugin = OSDictionary::withCapacity(1);
    OSNumber *ptype = OSNumber::withNumber((unsigned long long)MT2_PARSER_TYPE, 32);
    OSNumber *popts = OSNumber::withNumber((unsigned long long)MT2_PARSER_OPTIONS_BT, 32);   /* genuine BT value */
    OSString *plpath = OSString::withCString(MT2_MTHID_PLUGIN_PATH);
    if (!top || !mt || !plugin || !ptype || !popts || !plpath) {
        if (top) top->release(); if (mt) mt->release(); if (plugin) plugin->release();
        if (ptype) ptype->release(); if (popts) popts->release(); if (plpath) plpath->release();
        return 0;
    }
    plugin->setObject(MT2_MTHID_PLUGIN_GUID, plpath);
    mt->setObject("IOCFPlugInTypes", plugin);
    mt->setObject("parser-type", ptype);
    mt->setObject("parser-options", popts);
    mt->setObject(MT2_PROP_MTHID_DEVICE, kOSBooleanTrue);
    mt->setObject(MT2_PROP_HID_SERVICE_SUPPORT, kOSBooleanTrue);
    mt->setObject(MT2_PROP_MOMENTUM_SCROLL, kOSBooleanTrue);
    mt->setObject(MT2_PROP_SECONDARY_CLICK_CORNERS, kOSBooleanTrue);
    mt->setObject(MT2_PROP_FOUR_FINGER_GESTURES, kOSBooleanTrue);
    /* Physical-click reliability: BNB copies this key onto the spawned AMD, which reads it to open the
     * device-button gate — without it our click-sink posts are dropped (only tap-to-click survives). */
    mt->setObject(MT2_PROP_EXTRACT_BUTTON, kOSBooleanTrue);
    top->setObject("DefaultMultitouchProperties", mt);
    /* Do NOT seed "Product" here — IOHIDDevice::start overwrites it after; we set it post-start
     * (seedBnbIdentity). See explanation.md "Product re-seed after IOHIDDevice::start". */
    plpath->release(); ptype->release(); popts->release();
    plugin->release(); mt->release();
    return top;
}

/* genuine_host adapter: 7 of 9 ops are the shared gh_default_* (props via cfg.build_props); BT supplies
 * only interpose (the geometry vtable clone) + restore. NB the L2CAP delegate splice is NOT here — that
 * input seam is installed async by interposeTimerFired after BNB's interrupt channel appears. */
static int bt_gh_interpose(gh_host_t *h) {
    ((com_schmonz_MT2BTReader *)h->ctx)->installBnbGeometry(h->obj);  /* geometry vtable clone (class-gated) */
    return gBnbSpliceState.installed ? 0 : -1;  /* all-or-nothing: a failed clone is an interpose failure */
}
static void bt_gh_restore(gh_host_t *h) {
    ((com_schmonz_MT2BTReader *)h->ctx)->removeBnbGeometry(h->obj);   /* vtc_restore of the geometry clone */
}
static const gh_adapter_t kBtAdapter = {
    gh_default_alloc, gh_default_class_ok, gh_default_init_attach, bt_gh_interpose,
    gh_default_start, bt_gh_restore, gh_default_detach, gh_default_terminate, gh_default_release
};

bool com_schmonz_MT2BTReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    resetTransportState();

    /* Our provider is the matched IOBluetoothL2CAPChannel. */
    fChannel = (IOBluetoothL2CAPChannel *)provider;
    IOLog("MT2BTReader: [diag] start() PSM=%u chan->isInactive=%d\n",
          fChannel->getPSM(), fChannel->isInactive());

    /* The main flow, as a sequence of named steps: */
    if (!marshalSetupInGate()) return false;    /* bind channel + arm session, in-gate */
    if (fIsControl) {                           /* Path A runs only on the control channel */
        if (manualStartGenuineBnb())            /* host a genuine BNBTrackpadDevice */
            seedBnbIdentity();                  /* Product + battery-display gate on its node */
        if (fManualBnb)
            armInterposeTimer();                /* async: splice our shim once BNB's chan appears */
    }

    IOLog("MT2BTReader: bound L2CAP channel, enabled multitouch (0xF1), listening\n");
    registerService();
    return true;
}

/* Step: zero the per-connection fields (extracted from start()). */
void com_schmonz_MT2BTReader::resetTransportState() {
    fIsControl = false;
    fManualBnb = 0;
    fInterposeTimer = 0;
    fInterposeTries = 0;
    fReEnableCount = 0;
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

/* Step: manual-start a genuine BNBTrackpadDevice on this real L2CAP channel via the shared
 * genuine_host core (manual-start + class-gate + geometry vtable interpose live BEFORE start + start;
 * gh_start fully unwinds on failure). Run from this normal start() thread, NOT in the channel gate:
 * the BT-HID start chain can block on handshakes and blocking in-gate would deadlock. IOKit matching
 * can't be tricked, hence the manual start. Returns true (and sets fManualBnb) on success. */
bool com_schmonz_MT2BTReader::manualStartGenuineBnb() {
    static const gh_config_t cfg = { "BNBTrackpadDevice", "BNBTrackpadDevice", bt_build_bnb_props };
    if (gh_start(&fHost, &cfg, &kBtAdapter, this, fChannel) != 0) {
        IOLog("MT2BTReader: manual BNBTrackpadDevice host start FAILED\n");
        return false;
    }
    fManualBnb = (IOService *)fHost.obj;
    gGenuineBnb = fManualBnb;   /* publish for the interrupt reader + MT2Gesture sink (Phase 2) */
    (void)mt2_coordinator_activate(MT2_XPORT_BT, 0);   /* no-op seam (MT2 single-transport) */
    IOLog("MT2BTReader: manual BNBTrackpadDevice start OK\n");
    return true;
}

/* Step: seed the genuine node's identity now that fManualBnb exists. */
void com_schmonz_MT2BTReader::seedBnbIdentity() {
    /* Re-set Product post-start (IOHIDDevice::start clobbered it to the empty HID string) — see
     * explanation.md "Product re-seed after IOHIDDevice::start". */
    OSString *prod = OSString::withCString("Magic Trackpad 2");
    if (prod) { fManualBnb->setProperty("Product", prod); prod->release(); }
    /* Connect/disconnect bezel HUD = a TRACKPAD, not a mouse. IOBluetoothHIDDriver's connect timer
     * reads these keys and posts the event name to the BezelServices login plugin; NULL defaults a
     * pointing device to "MouseConnected" (→ mouse art). Because we manual-start BNBTrackpadDevice, the
     * genuine BNBTrackpadDriver personality (which declares these) never merges onto our node, so we
     * seed them here. Apple already ships /Library/Application Support/Apple/BezelServices/
     * AppleBluetoothMultitouch.plugin mapping (BNBTrackpadDevice, Connected/Disconnected/TrackpadOff)
     * -> BtTrackpad.pdf, so this is scoped to our node — a real Magic Mouse is untouched. See
     * docs/mt-stack/device-identity-map.md "Bezel HUD". */
    OSString *nc = OSString::withCString("Connected");
    if (nc) { fManualBnb->setProperty("ConnectionNotificationType", nc); nc->release(); }
    OSString *nd = OSString::withCString("Disconnected");
    if (nd) { fManualBnb->setProperty("DisconnectionNotificationType", nd); nd->release(); }
    OSString *np = OSString::withCString("TrackpadOff");
    if (np) { fManualBnb->setProperty("PoweredOffNotificationType", np); np->release(); }
    /* Battery display gate: the Trackpad pane's initWithHIDDevice: returns nil (→ 0%) if the node has
     * no "ExtendedFeatures" property, REGARDLESS of BatteryPercent. Publish a present-but-empty dict
     * purely to pass that presence gate — see explanation.md "the ExtendedFeatures presence gate". */
    OSDictionary *ef = OSDictionary::withCapacity(1);
    if (ef) { fManualBnb->setProperty("ExtendedFeatures", ef); ef->release(); }
    /* Fresh node → forget the prior battery so the next poll republishes onto it (else a same-value
     * reconnect skips the publish and the pane/menu read -1.0). */
    gLastBattPct = -1; gLastBattBnb = 0;
    bt_conntrace(CSM_BNB_FORMED, CSM_EV_BNB_LISTENING, fChannel, fManualBnb, 0, 0);
}

/* Step: arm the async poll for BNB's interrupt channel. It arrives ASYNCHRONOUSLY (publish
 * notification), so interposeTimerFired polls for it and installs the interpose once it (and BNB's
 * listenAt) are in place. Invariant: fInterposeTimer != 0 => it is attached to the workloop. */
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
    /* Clear our delegate only on the channel we actually set it on (the interrupt reader, PSM 19).
     * The control reader never set a delegate (the genuine BNBTrackpadDevice owns PSM 17's), so
     * clearing it there would clobber Apple's delegate on our unload. */
    if (self && !self->fIsControl && self->fChannel) self->fChannel->listenAt(self, 0);
    return kIOReturnSuccess;
}

/* In the channel's command gate: read BNB's current delegate callback (channel+0x110) and,
 * if BNB has registered it, swap in our shim (saving the original for the shim to forward to).
 * We overwrite ONLY the callback pointer; the target slot (+0x118) keeps BNB's own target,
 * which the shim forwards via the saved target. newDataIn re-reads +0x110 per packet, so the swap
 * takes effect immediately. Returns NotReady until BNB's listenAt has populated the slot. */
IOReturn com_schmonz_MT2BTReader::interposeInGate(OSObject * /*owner*/, void *arg0,
                                                  void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    int rc = mt2_splice_install(&kBtInterruptRow, ch, &mt2_splice_kext_ops, &gBtInterruptState);
    if (rc == MT2_SPLICE_NOT_READY) return kIOReturnNotReady;   /* slot not set / already ours */
    if (rc != MT2_SPLICE_OK) return kIOReturnError;
    gInterposedChannel = ch;
    IOLog("MT2BTReader: Path A interpose installed (origCb=%p origTgt=%p)\n",
          gBtInterruptState.saved_cb, gBtInterruptState.saved_target);
    bt_conntrace(CSM_INTERPOSED, CSM_EV_INTERPOSE_OK, ch, gGenuineBnb, gBtInterruptState.saved_cb, 0);
    return kIOReturnSuccess;
}

/* In-gate: restore BNB's original callback so the channel never calls our (freed) shim. */
IOReturn com_schmonz_MT2BTReader::restoreInGate(OSObject * /*owner*/, void *arg0,
                                                void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    mt2_splice_restore(ch, &kBtInterruptRow, &mt2_splice_kext_ops, &gBtInterruptState);
    return kIOReturnSuccess;
}

/* In the CONTROL channel's gate: save BNB's control-channel delegate and swap in bt_control_shim,
 * so we can sniff GET_REPORT(0x90) responses. Same save-and-swap as interposeInGate but on separate
 * state (gBtControlState). arg0 = the control channel (the control reader's own fChannel). Returns
 * NotReady until BNB has populated the slot. */
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

/* In-gate: restore BNB's original control-channel callback before we tear down. arg0 = channel. */
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
    static const uint8_t kEnable[] = { MT2_HIDP_SET_REPORT_FEATURE, MT2_ENABLE_REPORT_ID, 0x02, 0x01 };
    self->fChannel->sendTo((void *)kEnable, sizeof(kEnable), 0, self, 0, 0);
    return kIOReturnSuccess;
}

/* B1-drive probe: inject the 0x60/0x02 handler-create trigger into BNB's own data callback (the
 * interrupt seam's saved cb), which we saved when installing the interpose. Runs in the interrupt
 * channel's gate — the same
 * context BNB's real data callback runs in. arg0 = the interposed interrupt channel. */
IOReturn com_schmonz_MT2BTReader::triggerInGate(OSObject * /*owner*/, void *arg0,
                                                void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    /* 0xA1 transport byte + reportID 0x60 + 0x02, zero-padded. The minimal padded payload is enough to
     * drive BNB's processDesyncedMultitouchData -> startMultitouch -> createMultitouchHandler (S2.17,
     * no body-parse fault). */
    static const uint8_t kTrigger[16] = { 0xA1, MT2_TRIGGER_REPORT_ID, 0x02 };
    typedef void (*l2cap_cb_t)(IOService *, IOBluetoothL2CAPChannel *, unsigned short, void *);
    if (gBtInterruptState.saved_cb) {
        ((l2cap_cb_t)gBtInterruptState.saved_cb)(
            (IOService *)gBtInterruptState.saved_target, ch, (unsigned short)sizeof(kTrigger), (void *)kTrigger);
        IOLog("MT2BTReader: B1-drive injected 0x60/0x02 handler-create trigger into BNB\n");
    }
    return kIOReturnSuccess;
}

/* Dirty trick #2 (instance-scoped vtable clone): clone OUR transport instance's vtable and override
 * the getMultitouchReport slot with our geometry answerer. Instance-scoped, so only this transport's
 * vtable pointer changes — the shared class vtable (and a co-connected genuine MT1's transport) is
 * untouched. Must run BEFORE the create trigger so BNB's AMD bring-up query is answered. */
/* The safety gate (class must be BNBTrackpadDevice/BluetoothMultitouchTransport) and the two-slot
 * clone (data 0xcc8 + info 0xcd8, info first) are now the splice engine's — kBnbGeometryRow carries
 * the CLASS_NAME gate + both slots; the exact ABORT/FAILED/installed log behavior is preserved here. */
void com_schmonz_MT2BTReader::installBnbGeometry(void *transport) {
    if (!transport || gBnbSpliceState.installed) return;
    int rc = mt2_splice_install(&kBnbGeometryRow, transport, &mt2_splice_kext_ops, &gBnbSpliceState);
    if (rc == MT2_SPLICE_OK) {
        MT2_DLOG(1, "BNB geometry override installed on transport %p (data slot %lu, info slot %lu)",
                 transport, (unsigned long)GMR_SLOT_INDEX, (unsigned long)GMRINFO_SLOT_INDEX);
    } else if (rc == MT2_SPLICE_ABORT) {
        const char *cls = ((IOService *)transport)->getMetaClass()->getClassName();
        IOLog("MT2BTReader: BNB geometry ABORT — transport %p is '%s', not BNBTrackpadDevice\n",
              transport, cls ? cls : "(null)");
    } else {
        IOLog("MT2BTReader: BNB geometry override FAILED (rc=%d)\n", rc);
    }
}

void com_schmonz_MT2BTReader::removeBnbGeometry(void *transport) {
    mt2_splice_restore(transport, &kBnbGeometryRow, &mt2_splice_kext_ops, &gBnbSpliceState);
}

void com_schmonz_MT2BTReader::interposeTimerFired(OSObject *owner, IOTimerEventSource *ts) {
    com_schmonz_MT2BTReader *self = (com_schmonz_MT2BTReader *)owner;

    /* Phase 2 (interpose already installed): re-send the 0xF1 enable on the control channel a
     * few times to force the device back into multitouch mode after BNB's handleStart knocked
     * it to mouse mode (findings S2.11). self->fChannel is the control channel (this is the
     * control reader — the manual-start + this timer are armed only when fIsControl). */
    if (gInterposedChannel) {
        IOCommandGate *cg = self->fChannel
            ? ((IOBluetoothObject *)self->fChannel)->getCommandGate() : 0;

        /* Battery: install the CONTROL-channel interpose (once) so we can catch GET_REPORT(0x90)
         * responses. Retry each tick until BNB has set its control delegate; idempotent. */
        if (!gCtrlInterposedChannel && cg)
            cg->runAction(&com_schmonz_MT2BTReader::controlInterposeInGate, self->fChannel);

        /* Phase 2a: re-send the 0xF1 enable to force the device back to multitouch mode, retrying until
         * this connection's first frame (gSteadyConn == gConnId) — teardown-safe because it runs only
         * while gSteadyConn != gConnId. Cadence: fast (250ms) then gentle (1s). See explanation.md
         * "Reconnect re-enable: retry-until-first-frame, teardown-safe". */
        if (gSteadyConn != gConnId) {
            if (cg) cg->runAction(&com_schmonz_MT2BTReader::reEnableInGate, self);
            /* HANDLER_UP once the trigger has spawned BNB's AMD (visible at gGenuineBnb+0x1b0). */
            if (gGenuineBnb) {
                void *amd = *(void **)((uint8_t *)gGenuineBnb + BNB_HANDLER_OFF);
                static int gHandlerConn = -1;
                if (amd && gHandlerConn != gConnId) { gHandlerConn = gConnId;
                    bt_conntrace(CSM_HANDLER_UP, CSM_EV_HANDLER_SPAWNED, 0, gGenuineBnb, amd, 0); }
            }
            self->fReEnableCount++;
            if (self->fReEnableCount == 8)
                IOLog("MT2BTReader: initial re-enable push done; retrying gently until first frame\n");
            ts->setTimeoutMS(self->fReEnableCount < 8 ? 250 : 1000);
            return;
        }

        /* First multitouch frame seen for this connection -> confirmed in multitouch mode. Announce once,
         * then Phase 2b (steady state): poll the battery. bt_control_shim catches the GET_REPORT(0x90)
         * response and publishes "BatteryPercent". Slow cadence — a battery moves slowly. */
        if (self->fReEnableCount >= 0) {   /* -1 sentinel = already announced for this connection */
            IOLog("MT2BTReader: multitouch confirmed (first frame after %d enables)\n", self->fReEnableCount);
            bt_conntrace(CSM_MT_MODE, CSM_EV_MT_MODE_CONFIRMED, self->fChannel, gGenuineBnb, 0, 0);
            self->fReEnableCount = -1;
        }
        if (cg) cg->runAction(&com_schmonz_MT2BTReader::pollBatteryInGate, self);
        ts->setTimeoutMS(MT2_BATTERY_POLL_MS);
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
                self->installBnbGeometry(gGenuineBnb);
                /* Interpose is in (saved cb = BNB's data cb). Inject the handler-create trigger ONCE,
                 * in this same channel gate, so BNB spawns its own AMD before the re-enable phase.
                 * (gInterposedChannel is now set; this success branch runs once.) */
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
    /* Deregister from the engine: after this returns no session effect can reach our sink.
       No-op unless we are the active source (the interrupt reader) — the control reader's
       stop leaves the session alone; its delivery already dies via gGenuineBnb = 0 below. */
    if (gActiveMT2Gesture) gActiveMT2Gesture->connectionClosed(this);
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
    }
    /* Battery: restore BNB's original control-channel delegate too (separate save-and-swap), so the
     * about-to-be-freed bt_control_shim is never called. Same single-device global pattern. */
    if (gCtrlInterposedChannel) {
        IOCommandGate *gate = ((IOBluetoothObject *)gCtrlInterposedChannel)->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::controlRestoreInGate, gCtrlInterposedChannel);
        gCtrlInterposedChannel = 0;
    }
    /* Path A: tear down the manually-started genuine BNBTrackpadDevice before we go away, or it
     * outlives the channel it was started on. Plain terminate() + release(), NO waitQuiet — the BNB
     * never balances its start-time busy so a wait can never succeed; unload safety rests on the
     * in-gate restores ABOVE. See explanation.md "stop(): plain terminate + release, NO waitQuiet". */
    if (fManualBnb) {
        gGenuineBnb = 0;   /* stop the sink forwarding into it before we tear it down */
        gLastBattBnb = 0;  /* forget the torn-down node so a reused address can't false-match */
        /* Drain any in-flight sink delivery that read gGenuineBnb before the clear above, and
         * make the clear visible to later deliveries (the store alone is no barrier), BEFORE
         * gh_stop restores the vtable + terminates the BNB. Closes the (previously tolerated)
         * watchdog-flush-vs-teardown window on the control reader's stop, which never goes
         * through connectionClosed (only the interrupt reader is the registered source). */
        if (gActiveMT2Gesture) gActiveMT2Gesture->quiesceDelivery();
        /* genuine_host ordered teardown: removeBnbGeometry (vtc_restore the geometry clone) BEFORE
         * terminate — so BNB tears down through Apple's own code, not our override — then release. */
        gh_stop(&fHost, &kBtAdapter);
        fManualBnb = 0;
        IOLog("MT2BTReader: manual BNBTrackpadDevice terminate+release\n");
    }
    fChannel = 0;
    IOService::stop(provider);
}
