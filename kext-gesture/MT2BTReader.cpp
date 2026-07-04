/* MT2BTReader — in-kernel Bluetooth transport for the Magic Trackpad 2.
 *
 * A thin decoder. The MT2's multitouch frames are unreachable over BT from userspace
 * (the BT HID descriptor is boot-mouse-only; see the BT findings doc). We bind the
 * L2CAP channel directly, enable
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

/* Compiled as C++ under the kext toolchain (so is mt2_bt_decode.c), so these resolve
 * with C++ linkage on both sides — no extern "C". */
#include "mt2_bt_decode.h"
#include "mt2_battery.h"    /* mt2_parse_battery_report — shared pure decode of report 0x90 */
#include "mt2_pipeline.h"   /* MT2_EVENT_DRIVEN */
#include "mt2_log.h"           /* MT2_DLOG (runtime debug.mt2_log) */
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

/* The shipped architecture (design 2026-06-20-bnb-pathA-interpose-seam): on the control channel we
 * manual-start a genuine BNBTrackpadDevice, YIELD the interrupt-channel data delegate so BNB's own
 * listenAt succeeds (removing the §S2.5 panic vector), then interpose our MT2->MT1 shim on the
 * channel's delegate-callback slot. Apple's BNB drives cursor/gestures/prefpane; we condition + inject. */

/* Field offsets: canonical values + re/ commands live in ../src/mt2_stack.h. These are readable
 * local aliases so the numbers exist in exactly one place (no doc/build drift). */
#define BNB_HANDLER_OFF             MT2_OFF_BNB_AMD               /* AMD* (AppleMultitouchDevice*)  */
#define L2CAP_DELEGATE_CB_OFF       MT2_OFF_L2CAP_DELEGATE_CB     /* L2CAP delegate cb (+8 = target)*/
#define BNB_INTERRUPT_CHANNEL_OFF   MT2_OFF_BNB_INTERRUPT_CHANNEL /* BNBDevice::_interruptChannel    */

/* The genuine BNBTrackpadDevice the control reader manual-starts (Path A). The interpose
 * installer reads it (via +0xf0) to find BNB's interrupt channel, where it pokes our shim onto
 * the delegate-callback slot. Single device, single instance — a global mirrors the pattern of
 * gActiveMT2Gesture. NULL when no genuine device is up. */
IOService *gGenuineBnb = 0;

/* The PSM-19 (interrupt) reader instance — the session's active frame source (it calls
 * connectionEstablished(self)). The interpose shim feeds decoded frames through
 * gActiveMT2Gesture->submitFrame(gInterruptReader, ...), conditioned by the shared session and
 * routed to BNB's own spawned AMD (setBnbTarget). */
static IOService *gInterruptReader = 0;

/* Saved BNB delegate: triggerInGate injects the handler-create trigger through it (spawning BNB's
 * AMD) and stop()'s restore puts it back on the channel (single genuine device, like gGenuineBnb). */
static void *gOrigCb = 0;
static IOService *gOrigTarget = 0;
static IOBluetoothL2CAPChannel *gInterposedChannel = 0;

/* Battery poll: the CONTROL channel (PSM 17) delegate, interposed SEPARATELY from the interrupt one
 * so the interrupt shim's gOrigCb (used to forward touch frames / inject the create-trigger) is never
 * clobbered. Our control shim forwards EVERY control PDU to BNB's original (the control plane must stay
 * intact) and only peeks report 0x90 responses to our GET_REPORT(0x90) poll. Single genuine device, so
 * file-static globals mirror gGenuineBnb/gOrigCb. */
static void *gCtrlOrigCb = 0;
static IOService *gCtrlOrigTarget = 0;
static IOBluetoothL2CAPChannel *gCtrlInterposedChannel = 0;

/* BNB geometry: our transport instance's cloned vtable. */
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
/* The connId whose FIRST real multitouch frame we've seen (STEADY). Set by bt_interpose_shim on the first
 * decoded frame; read by interposeTimerFired to gate the multitouch-enable RETRY. Keyed on gConnId, so a
 * fresh connection (gConnId bumped) is automatically < STEADY until it produces a frame — no reset needed.
 * This is the whole teardown-safety of the reconnect fix: we re-enable ONLY while gSteadyConn != gConnId
 * (pre-first-frame bring-up), so a working device (already STEADY) powering off never triggers a re-enable
 * during its teardown — which was exactly the v1 bug (v1 re-enabled on any mouse-mode report). */
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

/* Battery bridge. The MT2 reports battery as the standard Apple Power-Device INPUT report id 0x90 =
 * [id 0x90][status flags][capacity 0-100] (Usage Page 0x84 Power Device / 0x85 Battery System, capacity
 * = Usage 0x65). Verified live on both transports (e.g. `90 05 64` = 100%; byte[1] bit = charging).
 * Publish the capacity as the "BatteryPercent" OSNumber (0-100) on the genuine BNB node — EXACTLY the
 * property the Trackpad pane reads: -[AppleBluetoothHIDDevice batteryPercent] does
 * IORegistryEntryCreateCFProperty(node,"BatteryPercent")->unsignedLongValue. This bypasses BNB's own
 * MT1-shaped voltage/chemistry model (getExtendedReport, which the MT2 can't answer -> "No extended
 * features"). setProperty is registry-lock-guarded, safe from this newDataIn context. Publish only on
 * change (avoids churning the registry if 0x90 repeats). */
/* Publish dedup, keyed on BOTH the value AND the node instance. Each BT reconnect manual-starts a
 * FRESH BNBTrackpadDevice (a brand-new registry node with no BatteryPercent); keying on value alone
 * meant a same-value reconnect (e.g. 100 -> 100) skipped the setProperty on the fresh node, so the
 * pane/menu read -1.0 (no info) after every reconnect. Keying on the node too forces a republish
 * onto each new node. gLastBattBnb is reset in start()/stop() so a malloc-reused address can't
 * false-match. */
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

/* Diagnostic: log each DISTINCT report id the shim sees, once. On a load this reveals whether the
 * battery report 0x90 actually arrives on the interrupt channel (passive watch works) or not (we'd
 * need to poll it). Cheap — the device streams only a handful of distinct ids. */
static void mt2_diag_report_id(uint8_t id) {
    static uint8_t seen[32];
    if (seen[id >> 3] & (uint8_t)(1u << (id & 7))) return;
    seen[id >> 3] |= (uint8_t)(1u << (id & 7));
    MT2_DLOG(1, "shim saw report id 0x%02x", (unsigned)id);
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

    /* Battery: the MT2 does NOT stream report 0x90 on the interrupt channel (proven — this never
     * fired), so battery is polled on the CONTROL channel (bt_control_shim + the poll timer). This
     * peek is kept as a cheap safety net in case a firmware ever pushes it here; distinct id from the
     * multitouch stream (0x31), so it never disturbs the touch decode below. */
    if (rlen > 0) mt2_diag_report_id(rep[0]);
    mt2_maybe_publish_battery(rep, rlen);

    touch_frame_t tf;
    int drc = mt2_bt_decode(rep, rlen, &tf);
    { static bool once = false; if (!once) { once = true;
        IOLog("MT2BTReader: [diag] first shim hit len=%u b0=0x%02x decode=%d\n",
              length, length ? b[0] : 0, drc); } }
    if (drc != 0) return;                                                 /* not multitouch — drop */

    /* STEADY: first real multitouch frame of this connection = the pad is streaming end-to-end. Recording
     * gSteadyConn here STOPS the enable-retry loop in interposeTimerFired (file-scope; see its decl). */
    if (gSteadyConn != gConnId) { gSteadyConn = gConnId;
        bt_conntrace(CSM_STEADY, CSM_EV_FRAME, channel, gGenuineBnb, 0, 0); }

    /* EDGE-CLAMP PROBE (debug.mt2_log>=2): per-frame decoded contact-0 X/Y + ts, to correlate the
     * faithful decoded position against the recognizer's norm.x (re/mt-contacts). If norm.x saturates
     * to 0/1 while decoded x is still moving -> a downstream clamp band (in MultitouchSupport's
     * report-X -> position step, using our published geometry). Smooth no-dwell sweep; tail = edge. */
    if (tf.ntouches > 0)
        MT2_DLOG(2, "edge x=%d y=%d ts=%u", tf.touches[0].x, tf.touches[0].y, bt_uptime_ms());

    /* Feed: route through the SHARED mt2_session — drop-lifted, MakeTouch/Touching/BreakTouch
     * lifecycle, liftoff — with the session sink targeting BNB's own spawned AMD (setBnbTarget). A
     * raw direct feed (mt1_encode -> handleTouchFrame, no lifecycle states) janked the cursor: the
     * recognizer never saw proper contact-start/lift transitions. */
    if (gActiveMT2Gesture && gInterruptReader) {
        void *amd = gGenuineBnb ? *(void **)((uint8_t *)gGenuineBnb + BNB_HANDLER_OFF) : 0;
        gActiveMT2Gesture->setBnbTarget(amd);
        gActiveMT2Gesture->submitFrame(gInterruptReader, &tf);
    }
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
     * interrupt channel). Strip the 0xA1 transport byte for the id, matching the interrupt diag. */
    if (length > 0) mt2_diag_report_id((length > 1 && b[0] == 0xA1) ? b[1] : b[0]);
    mt2_maybe_publish_battery(data, length);
    /* Forward to BNB's real delegate — the control plane depends on seeing every PDU. */
    if (gCtrlOrigCb) ((bt_l2cap_cb_t)gCtrlOrigCb)(gCtrlOrigTarget, channel, length, data);
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

    /* PSM 19 (interrupt): genuine BNBTrackpadDevice owns this channel's delegate (its handleStart
     * listenAt's it). We do NOT listen on it ourselves — if we held it, BNB's listenAt would return
     * 0xe00002bc -> forced teardown -> panic (findings §S2.5/§S2.6). We interpose BNB's delegate later
     * (interposeInGate) instead of owning the channel. */
    self->fIsControl = (psm == 0x11);
    /* DEFER the 0xF1 multitouch enable (root cause 2026-06-21): firing 0xF1 on PSM 17 BEFORE the
     * channel reaches OPEN makes sendTo block ~14s; the device tears the link down meanwhile (it never
     * got the genuine listenAt + waitForChannelState(OPEN) acceptance) -> channel inactive -> BNB
     * attach fails -> flap. So we let BNB's handleStart accept PSM 17 first; the phase-2 reEnable timer
     * sends 0xF1 after both channels are OPEN (RE §6). */
    IOLog("MT2BTReader: setup on PSM=%u\n", psm);
    /* A control channel coming up starts a new connection attempt — bump the id and mark CONTROL_UP. */
    if (psm == 0x11) { gConnId++; bt_conntrace(CSM_CONTROL_UP, CSM_EV_CONTROL_OPEN, self->fChannel, 0, 0, 0); }
    return kIOReturnSuccess;
}

/* B1: build the OSDictionary passed to BNBTrackpadDevice::init. multitouchProperties() does
 * getProperty("DefaultMultitouchProperties"), so init's property table must carry that key with the
 * genuine config — parser-type 1000 + the MultitouchHID plugin IOCFPlugInTypes — so the AppleMultitouchDevice
 * BNB spawns is recognized by MultitouchSupport (gets a user client, drives the cursor). Mirrors the
 * BNBTrackpadDriver personality (re/plist AppleBluetoothMultitouch). Caller inits with it, then releases.
 * Returns NULL on alloc failure. Returns the dict as void* (the gh_config_t build_props signature;
 * gh_default_init_attach casts back). */
static void *bt_build_bnb_props(void) {
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
    mt->setObject(MT2_PROP_EXTRACT_BUTTON, kOSBooleanTrue);
    top->setObject("DefaultMultitouchProperties", mt);
    /* NB: do NOT seed "Product" in the init dict — IOHIDDevice::start (BNB's superclass) overwrites it
     * from the empty HID product string AFTER this dict is applied (verified live: node Product="" despite
     * the seed). We set Product post-start instead, in start() once fManualBnb exists. */
    plpath->release(); ptype->release(); popts->release();
    plugin->release(); mt->release();
    return top;
}

/* ---- genuine_host adapter: the seven generic ops are the shared gh_default_* (gh_default_adapter.h),
 * with bt_build_bnb_props supplied via cfg.build_props; BT supplies only interpose (the geometry vtable
 * clone) + restore. The L2CAP delegate poke (the input seam) is NOT here — it is installed async by
 * interposeTimerFired after BNB's interrupt channel appears. interpose/restore call the reader's
 * installBnbGeometry/removeBnbGeometry, so they read h->ctx. ---- */
static int bt_gh_interpose(gh_host_t *h) {
    ((com_schmonz_MT2BTReader *)h->ctx)->installBnbGeometry(h->obj);  /* geometry vtable clone (class-gated) */
    return gBnbVtableCloned ? 0 : -1;       /* all-or-nothing: a failed clone is an interpose failure */
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
    if (fIsControl) {
        /* Host a genuine BNBTrackpadDevice via the shared genuine_host core: manual-start + class-gate +
         * geometry vtable interpose (live BEFORE start, so the AMD bring-up's first cacheDeviceProperties
         * sees real geometry) + start. gh_start fully unwinds on any failure (restore-before-terminate). */
        static const gh_config_t cfg = { "BNBTrackpadDevice", "BNBTrackpadDevice", bt_build_bnb_props };
        if (gh_start(&fHost, &cfg, &kBtAdapter, this, fChannel) == 0) {
            fManualBnb = (IOService *)fHost.obj;
            gGenuineBnb = fManualBnb;   /* publish for the interrupt reader + MT2Gesture sink (Phase 2) */
            (void)mt2_coordinator_activate(MT2_XPORT_BT, 0);   /* no-op seam (MT2 single-transport) */
            IOLog("MT2BTReader: manual BNBTrackpadDevice start OK\n");
            /* IOHIDDevice::start clobbered Product to the empty HID product string; re-set it now so
             * System Report / any Product reader shows the genuine name. (Distinct from the BT-pane
             * displayName, which blued owns — set by mt2_set_btname.) USB needs no equivalent: there the
             * genuine AMD copies the device's real USB iProduct descriptor ("Magic Trackpad"). */
            OSString *prod = OSString::withCString("Magic Trackpad 2");
            if (prod) { fManualBnb->setProperty("Product", prod); prod->release(); }
            /* Battery display gate: the Trackpad pane reads battery via
             * -[AppleBluetoothHIDDevice withBluetoothDevice:], whose initWithHIDDevice: (IOBluetooth
             * @0x114d8) does `if (IORegistryEntryCreateCFProperty(node,"ExtendedFeatures")==nil){dealloc;
             * return nil;}` — so with no ExtendedFeatures the wrapper is nil and batteryPercent returns 0
             * REGARDLESS of our published BatteryPercent (RE'd 2026-07-01, tools/mt2_panebattery_probe).
             * Genuine MT1 sets it from real extended feature reports; the MT2 has none. Publish a
             * present-but-empty dict purely to pass that presence gate — batteryPercent then reads our
             * "BatteryPercent" off this same node (it does not consult the dict's contents). */
            OSDictionary *ef = OSDictionary::withCapacity(1);
            if (ef) { fManualBnb->setProperty("ExtendedFeatures", ef); ef->release(); }
            /* Fresh node -> forget the prior battery so the next poll republishes onto it (else a
             * same-value reconnect skips the publish and the pane/menu read -1.0). */
            gLastBattPct = -1; gLastBattBnb = 0;
            bt_conntrace(CSM_BNB_FORMED, CSM_EV_BNB_LISTENING, fChannel, fManualBnb, 0, 0);
        } else {
            IOLog("MT2BTReader: manual BNBTrackpadDevice host start FAILED\n");
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

/* In the CONTROL channel's gate: save BNB's control-channel delegate and swap in bt_control_shim,
 * so we can sniff GET_REPORT(0x90) responses. Same save-and-swap as interposeInGate but on separate
 * globals (gCtrlOrigCb/…). arg0 = the control channel (the control reader's own fChannel). Returns
 * NotReady until BNB has populated the slot. */
IOReturn com_schmonz_MT2BTReader::controlInterposeInGate(OSObject * /*owner*/, void *arg0,
                                                         void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    uint8_t *c = (uint8_t *)ch;
    void **cbslot = (void **)(c + L2CAP_DELEGATE_CB_OFF);
    void *cur = *cbslot;
    if (!cur || cur == (void *)&bt_control_shim) return kIOReturnNotReady;   /* not set yet / already ours */
    gCtrlOrigCb = cur;
    gCtrlOrigTarget = (IOService *)*(void **)(c + L2CAP_DELEGATE_CB_OFF + 8); /* +0x118 target */
    *cbslot = (void *)&bt_control_shim;
    gCtrlInterposedChannel = ch;
    IOLog("MT2BTReader: battery control interpose installed (origCb=%p origTgt=%p)\n",
          gCtrlOrigCb, gCtrlOrigTarget);
    return kIOReturnSuccess;
}

/* In-gate: restore BNB's original control-channel callback before we tear down. arg0 = channel. */
IOReturn com_schmonz_MT2BTReader::controlRestoreInGate(OSObject * /*owner*/, void *arg0,
                                                       void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    void **cbslot = (void **)((uint8_t *)ch + L2CAP_DELEGATE_CB_OFF);
    if (*cbslot == (void *)&bt_control_shim && gCtrlOrigCb) *cbslot = gCtrlOrigCb;
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

/* B1-drive probe: inject the 0x60/0x02 handler-create trigger into BNB's own data callback (gOrigCb),
 * which we saved when installing the interpose. Runs in the interrupt channel's gate — the same
 * context BNB's real data callback runs in. arg0 = the interposed interrupt channel. */
IOReturn com_schmonz_MT2BTReader::triggerInGate(OSObject * /*owner*/, void *arg0,
                                                void * /*a1*/, void * /*a2*/, void * /*a3*/) {
    IOBluetoothL2CAPChannel *ch = (IOBluetoothL2CAPChannel *)arg0;
    /* 0xA1 transport byte + reportID 0x60 + 0x02, zero-padded. The minimal padded payload is enough to
     * drive BNB's processDesyncedMultitouchData -> startMultitouch -> createMultitouchHandler (S2.17,
     * no body-parse fault). */
    static const uint8_t kTrigger[16] = { 0xA1, MT2_TRIGGER_REPORT_ID, 0x02 };
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
    if (!transport || gBnbVtableCloned) return;
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
        IOCommandGate *cg = self->fChannel
            ? ((IOBluetoothObject *)self->fChannel)->getCommandGate() : 0;

        /* Battery: install the CONTROL-channel interpose (once) so we can catch GET_REPORT(0x90)
         * responses. Retry each tick until BNB has set its control delegate; idempotent. */
        if (!gCtrlInterposedChannel && cg)
            cg->runAction(&com_schmonz_MT2BTReader::controlInterposeInGate, self->fChannel);

        /* Phase 2a: re-send the 0xF1 enable to force the device out of the basic HID mouse mode BNB's
         * handleStart knocked it into (findings S2.11) — and KEEP re-sending until this connection's first
         * real multitouch frame (gSteadyConn == gConnId). On a rapid/unstable reconnect the enable
         * setReport can fail (channel-not-ready, 0xe00002bc) for the first few seconds; the old fixed
         * 8-sends-then-give-up window would then miss it and leave the device stuck in mouse mode = no
         * cursor until a manual tap ([[bt-reconnect-enable-fails]]). Retrying until the frame flows fixes
         * that. TEARDOWN-SAFE: we only run while gSteadyConn != gConnId, so a device that already reached
         * STEADY and is now powering off never re-enters this branch — that (re-enabling on a working
         * device's power-off) was the v1 regression. Cadence: fast (250ms) for the initial push, then gentle
         * (1s) so a genuinely-stuck-but-connected device isn't spammed. */
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
                /* Interpose is in (gOrigCb = BNB's data cb). Inject the handler-create trigger ONCE,
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
    /* Battery: restore BNB's original control-channel delegate too (separate save-and-swap), so the
     * about-to-be-freed bt_control_shim is never called. Same single-device global pattern. */
    if (gCtrlInterposedChannel) {
        IOCommandGate *gate = ((IOBluetoothObject *)gCtrlInterposedChannel)->getCommandGate();
        if (gate) gate->runAction(&com_schmonz_MT2BTReader::controlRestoreInGate, gCtrlInterposedChannel);
        gCtrlInterposedChannel = 0;
        gCtrlOrigCb = 0; gCtrlOrigTarget = 0;
    }
    /* Path A: tear down the manually-started genuine BNBTrackpadDevice before we go away, or it
     * outlives the channel it was started on.
     *
     * NO waitQuiet (RE'd 2026-06-23, see docs/mt-stack/open-questions.md): the manually-started BNB
     * sits at busyState=1 for its ENTIRE life — its genuine connect lifecycle never completes
     * (deviceReady is never reached in our hybrid flow; the 5s "Forcing MT restart" watchdog cycles),
     * so the start-time busy is never balanced. terminate() can't drop it: probe showed busy=1 before
     * terminate and still busy=1 after a full 8s waitQuiet (AMD child busy=0, so it's BNB's own). A
     * waitQuiet here can therefore NEVER succeed — it only stalls every disconnect for the full bound.
     * Unload safety rests on the in-gate delegate + vtable restores ABOVE (not on quiescence); the
     * async termination completes after release(). So: plain terminate() + release(), no wait. */
    if (fManualBnb) {
        gGenuineBnb = 0;   /* stop the sink forwarding into it before we tear it down */
        gLastBattBnb = 0;  /* forget the torn-down node so a reused address can't false-match */
        /* genuine_host ordered teardown: removeBnbGeometry (vtc_restore the geometry clone) BEFORE
         * terminate — so BNB tears down through Apple's own code, not our override — then release. */
        gh_stop(&fHost, &kBtAdapter);
        fManualBnb = 0;
        IOLog("MT2BTReader: manual BNBTrackpadDevice terminate+release\n");
    }
    fChannel = 0;
    IOService::stop(provider);
}
