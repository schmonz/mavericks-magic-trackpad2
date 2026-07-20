/*
 * mavericks_amd_terminal — fabricated AppleMultitouchDevice build/teardown.
 *
 * Recovered from 2c900e9^:kext-gesture/MavericksVoodooInputHost.cpp (the AMD construction that
 * shipped pre-2026-06-24 synthetic-removal). Refactored into standalone build/teardown
 * functions taking the provider nub; get/set stubs now thin glue over the host-tested
 * mavericks_synth_report instead of the old inline g_reg[] echo + mt2_fill_geometry_report.
 * All RE'd constants, property keys/values, plugin UUID/path, and dict shapes are
 * preserved verbatim from the old source.
 *
 * 2026-07-13: converted from module-static g_regs/g_shell to a per-build heap context
 * (mavericks_amd_terminal_ctx) so N independent fabricated AMDs can coexist. Each mux instance
 * will own one ctx. Report stubs read regs from the ctx registered as their target.
 */
#include "MavericksAMDTerminal.h"
#include "../src/mavericks_synth_teardown.h"
#include "mavericks_amd_terminal_encode.h"
#include "mt2_log.h"           /* MT2_DLOG (runtime debug.mt2_log) */
#include "MavericksHIDShell.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSMetaClass.h>
extern "C" {
#include "mavericks_synth_report.h"
}
/* mavericks_stack.h: MT2_PROP_EXTRACT_BUTTON — must precede start() */
#include "../src/mavericks_stack.h"

/* ---- per-build context (replaces module-static g_regs / g_shell) ----------------------------- */

struct mavericks_amd_terminal_ctx {
    mavericks_synth_regs_t          regs;
    com_schmonz_MavericksHIDShell  *shell;
    AppleMultitouchDevice    *amd;
    bool                      ready;  /* feed fence: amd() returns NULL until built, and again once teardown starts */
    IOWorkLoop               *wl;     /* retained AMD workloop; released LAST (VoodooInput invariant) */
};

/* ---- handler stubs (RE'd layout: buf at b+1; length at *(u32*)(b+0x204)) ------------------- */

/* AMDDeviceReportStruct layout (RE'd from Apple staticGet/SetReportHandler):
 *   off 0x000  uint8  reportID
 *   off 0x001  uint8  buf[0x203]   (report data)
 *   off 0x204  uint32 length
 * enableStub: hidd calls this to enable/disable multitouch delivery. Log and return OK. */
static int enableStub(bool enable, void *t) {
    (void)t;
    IOLog("mavericks_amd_terminal: ENABLE-MT enable=%d\n", (int)enable);
    return 0;
}

/* getReportStub: routes through mavericks_synth_answer_report (host-tested). For reports
 * that must be skipped (0xDB Multitouch ID), return kIOReturnUnsupported so hidd
 * moves on cleanly. Reads regs from ctx registered as the target. */
static int getReportStub(AMDDeviceReportStruct *r, unsigned char id, void *t) {
    (void)id;
    mavericks_amd_terminal_ctx *c = (mavericks_amd_terminal_ctx *)t;
    unsigned char *b = (unsigned char *)r;
    unsigned char rid = b[0];
    unsigned char *o = b + 1;                       /* response data buffer */
    unsigned int *lenp = (unsigned int *)(b + 0x204);
    unsigned int n = 0;
    mavericks_synth_rc_t rc = mavericks_synth_answer_report(&c->regs, rid, o, &n);
    if (rc == MAVERICKS_SYNTH_SKIP) {
        IOLog("mavericks_amd_terminal: GET-REPORT id=0x%02x -> SKIP (kIOReturnUnsupported)\n",
              (unsigned)rid);
        return (int)0xe00002c7;                     /* kIOReturnUnsupported */
    }
    *lenp = n;
    IOLog("mavericks_amd_terminal: GET-REPORT id=0x%02x -> %u bytes\n", (unsigned)rid, n);
    return 0;
}

/* setReportStub: remember the 1-byte value hidd SETs per reportID so a later GET
 * echoes it back (hidd SETs 0xC8/0xDC/0xDD then GETs them and may disable gestures
 * if the GET fails). Routes through mavericks_synth_note_set (host-tested). Reads regs
 * from ctx registered as the target. */
static int setReportStub(AMDDeviceReportStruct *r, unsigned char id, void *t) {
    (void)id;
    mavericks_amd_terminal_ctx *c = (mavericks_amd_terminal_ctx *)t;
    unsigned char *b = (unsigned char *)r;
    mavericks_synth_note_set(&c->regs, b[0], b[1]);
    IOLog("mavericks_amd_terminal: SET-REPORT typeArg=%u reportID=0x%02x len=%u b1=0x%02x\n",
          (unsigned)id, (unsigned)b[0], *(unsigned int *)(b + 0x204), (unsigned)b[1]);
    return 0;
}

/* ---- MavericksHIDShell property dict (BT Magic Trackpad identity; mirrors old makeHidProps) ------- */

static const char *xport_str(mavericks_amd_terminal_transport_t xport) {
    return (xport == MAVERICKS_AMD_TERMINAL_XPORT_USB) ? "USB" : "Bluetooth";
}

static OSDictionary *makeHidProps(mavericks_amd_terminal_transport_t xport) {
    OSDictionary *p = OSDictionary::withCapacity(8);
    if (!p) return 0;
    struct { const char *k; unsigned v; } nums[] = {
        {"VendorID", 1452}, {"ProductID", 782}, {"VendorIDSource", 2},
        {"PrimaryUsagePage", 0x01}, {"PrimaryUsage", 0x02},
    };
    for (unsigned i = 0; i < sizeof(nums)/sizeof(nums[0]); i++) {
        OSNumber *n = OSNumber::withNumber(nums[i].v, 32);
        if (n) { p->setObject(nums[i].k, n); n->release(); }
    }
    OSString *t = OSString::withCString(xport_str(xport));
    if (t) { p->setObject("Transport", t); t->release(); }
    OSString *pr = OSString::withCString("Magic Trackpad");
    if (pr) { p->setObject("Product", pr); pr->release(); }
    OSString *mf = OSString::withCString("Apple Inc.");
    if (mf) { p->setObject("Manufacturer", mf); mf->release(); }
    return p;
}

/* ---- public API ------------------------------------------------------------------------------ */

mavericks_amd_terminal_ctx *mavericks_amd_terminal_build(IOService *nub, mavericks_amd_terminal_transport_t transport) {
    /* 0. Allocate the per-build context. */
    mavericks_amd_terminal_ctx *ctx = IONew(mavericks_amd_terminal_ctx, 1);
    if (!ctx) {
        IOLog("mavericks_amd_terminal: context alloc failed\n");
        return 0;
    }
    bzero(ctx, sizeof *ctx);

    /* 1. Zero the echo-register table. */
    mavericks_synth_regs_init(&ctx->regs);

    /* 2. Attach + start the MavericksHIDShell under nub so Apple's
     *    AppleMultitouchHIDEventDriver can match it and produce an IOHIDEventService
     *    in our subtree. AppleMultitouchDevice::start (non-fake) wires its actuation
     *    wrapper to event drivers found in the provider subtree — the shell must be
     *    here BEFORE the AMD starts. Best-effort: failure is logged but the build
     *    continues (the AMD may still start in lenient mode). */
    ctx->shell = 0;
    {
        com_schmonz_MavericksHIDShell *hid = new com_schmonz_MavericksHIDShell;
        OSDictionary *hp = makeHidProps(transport);
        if (hid && hp && hid->init(hp)) {
            if (hid->attach(nub) && hid->start(nub)) {
                ctx->shell = hid;
                IOLog("mavericks_amd_terminal: MT1 HID shell started under nub\n");
            } else {
                IOLog("mavericks_amd_terminal: MT1 HID shell attach/start FAILED\n");
                hid->detach(nub);
                hid->release();
                hid = 0;
            }
        } else {
            IOLog("mavericks_amd_terminal: MT1 HID shell init/alloc FAILED\n");
            if (hid) hid->release();
        }
        if (hp) hp->release();
    }

    /* 3. Allocate the real AppleMultitouchDevice via the IOKit class registry. */
    OSObject *o = OSMetaClass::allocClassWithName("AppleMultitouchDevice");
    if (!o) {
        IOLog("mavericks_amd_terminal: allocClassWithName(AppleMultitouchDevice) NULL\n");
        if (ctx->shell) { ctx->shell->terminate(); ctx->shell->release(); ctx->shell = 0; }
        IODelete(ctx, mavericks_amd_terminal_ctx, 1);
        return 0;
    }
    IOService *dev = OSDynamicCast(IOService, o);
    if (!dev) {
        IOLog("mavericks_amd_terminal: alloc is not an IOService\n");
        o->release();
        if (ctx->shell) { ctx->shell->terminate(); ctx->shell->release(); ctx->shell = 0; }
        IODelete(ctx, mavericks_amd_terminal_ctx, 1);
        return 0;
    }
    /* Layout-compatible reinterpret: AppleMultitouchDevice single-inherits IOService
     * at offset 0, so the pointer is identical. The shim adds only non-virtual method
     * declarations resolved to exported symbols; it does not alter the real vtable. */
    ctx->amd = (AppleMultitouchDevice *)dev;

    /* 4. Build init props dict with IsFake=false (the STRICT path: see old source comment).
     *    Belt-and-suspenders: setProperty after init in case init() did not adopt the dict. */
    OSDictionary *props = OSDictionary::withCapacity(2);
    if (!props) {
        IOLog("mavericks_amd_terminal: props dict alloc failed\n");
        dev->release();
        ctx->amd = 0;
        if (ctx->shell) { ctx->shell->terminate(); ctx->shell->release(); ctx->shell = 0; }
        IODelete(ctx, mavericks_amd_terminal_ctx, 1);
        return 0;
    }
    props->setObject("IsFake", kOSBooleanFalse);   /* bypass start() provider cast */
    bool ok = dev->init(props);
    props->release();
    IOLog("mavericks_amd_terminal: AppleMultitouchDevice init -> %d (dev=%p)\n", ok, dev);
    if (!ok) {
        dev->release();
        ctx->amd = 0;
        if (ctx->shell) { ctx->shell->terminate(); ctx->shell->release(); ctx->shell = 0; }
        IODelete(ctx, mavericks_amd_terminal_ctx, 1);
        return 0;
    }
    /* Belt-and-suspenders: ensure IsFake is in the property table even if init() did
     * not adopt our dict wholesale. start() reads it via getProperty. */
    dev->setProperty("IsFake", kOSBooleanFalse);

    /* Physical-click reliability: start() reads ExtractAndPostDeviceButtonState==true and sets
     * a flag that makes handlePointerEventFromDevice dispatch the device button immediately on
     * press/release edges — not only when a later motion frame ORs it in. Without it quick
     * stationary taps (no motion between press and release) are dropped (RE'd live). Must be
     * present before start() runs its gate check, hence set alongside IsFake. */
    dev->setProperty(MT2_PROP_EXTRACT_BUTTON, kOSBooleanTrue);

    /* 5. Install handler stubs before start(), with ctx as target so each stub operates on
     *    the per-build register table (enables N independent AMDs to coexist). */
    ctx->amd->setEnableMultitouchHandler(&enableStub, ctx);
    ctx->amd->setGetReportHandler(&getReportStub, ctx);
    ctx->amd->setSetReportHandler(&setReportStub, ctx);

    /* 6. Attach to nub. */
    if (!dev->attach(nub)) {
        IOLog("mavericks_amd_terminal: device attach failed\n");
        dev->release();
        ctx->amd = 0;
        if (ctx->shell) { ctx->shell->terminate(); ctx->shell->release(); ctx->shell = 0; }
        IODelete(ctx, mavericks_amd_terminal_ctx, 1);
        return 0;
    }

    /* 7. Set RE'd properties VERBATIM (must be before start() for the ones it reads;
     *    the rest are set after start() mirrors the old source's order). */

    /* MultitouchSupport::MTDeviceIsBuiltIn reads "MT Built-In"; "Driver is Ready" is
     * cached by mt_CachePropertiesForDevice. Set before start() so they are present
     * when the device registers. */
    /* Transport-match the pane/MultitouchSupport chrome: USB must not claim Bluetooth (battery is
     * BT-only; the BT device list is BT-only). Set on the AMD too, not just the HID shell. */
    dev->setProperty("Transport", xport_str(transport));
    dev->setProperty("MT Built-In", kOSBooleanTrue);
    dev->setProperty("Driver is Ready", kOSBooleanTrue);

    /* 8. start() */
    if (!dev->start(nub)) {
        IOLog("mavericks_amd_terminal: device start FAILED\n");
        dev->detach(nub);
        dev->release();
        ctx->amd = 0;
        if (ctx->shell) { ctx->shell->terminate(); ctx->shell->release(); ctx->shell = 0; }
        IODelete(ctx, mavericks_amd_terminal_ctx, 1);
        return 0;
    }

    /* Post-start properties (RE'd from DefaultMultitouchProperties / BNBTrackpadDriver
     * personality; values and keys preserved verbatim). */

    /* hidd instantiates the MultitouchHID.plugin via IOCFPlugInTypes to open and adopt
     * the device. Without this dict hidd never adopts us. UUID and plugin path RE'd
     * verbatim from a running BNBTrackpadDevice in IORegistry. */
    {
        OSDictionary *plug = OSDictionary::withCapacity(1);
        OSString *path = OSString::withCString(
            "AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin");
        if (plug && path) {
            plug->setObject("0516B563-B15B-11DA-96EB-0014519758EF", path);
            dev->setProperty("IOCFPlugInTypes", plug);
        }
        if (path) path->release();
        if (plug) plug->release();
    }

    /* parser-type/options tell MultitouchSupport how to parse the device; MTHIDDevice /
     * HIDServiceSupport flag it as a real MT HID trackpad. Values RE'd verbatim. */
    {
        OSNumber *pt = OSNumber::withNumber((unsigned long long)1000, 32);
        if (pt) { dev->setProperty("parser-type", pt); pt->release(); }
        OSNumber *po = OSNumber::withNumber((unsigned long long)47, 32);
        if (po) { dev->setProperty("parser-options", po); po->release(); }
    }
    dev->setProperty("TrackpadFourFingerGestures", kOSBooleanTrue);
    dev->setProperty("TrackpadMomentumScroll", kOSBooleanTrue);
    dev->setProperty("MTHIDDevice", kOSBooleanTrue);
    dev->setProperty("HIDServiceSupport", kOSBooleanTrue);
    dev->setProperty("TrackpadSecondaryClickCorners", kOSBooleanTrue);
    /* Tell the system to treat this as a TRACKPAD (cursor/gestures), not a generic
     * digitizer. hidd forwards our touches as raw DigitizerEvents but runs no
     * gesture/cursor recognition without this (stock BNBTrackpadDriver sets it). */
    dev->setProperty("HIDDefaultBehavior", "Trackpad");

    /* GESTURE/CURSOR ACTIVATION (RE'd from MTTrackpadHIDManager::determineHIDManagerSettings
     * in MultitouchHID.plugin): the userspace gesture recognizer (hidd) builds its
     * trackpad settings + chord-gesture-set by reading a preferences dictionary from this
     * device's IORegistry entry. determineHIDManagerSettings reads "TrackpadUserPreferences"
     * FIRST and, only if that key is ABSENT, falls back to "MultitouchPreferences"
     * (0x1c3ae/0x1c3c4: the testq/jne skips the fallback whenever the first key is present).
     * With NEITHER key present it runs a bare-defaults path that leaves the chord set empty
     * -> no chord ever commits -> no cursor/tap/scroll/gesture output.
     *
     * We seed "MultitouchPreferences" (NOT "TrackpadUserPreferences") on purpose: the genuine
     * settings-push pipeline (login daemon / prefpane -> BNB setProperties -> _setMultitouch
     * Preferences -> poked +0x1b0 -> AppleMultitouchDevice::setPreferences) writes/merges
     * the user's live prefs into exactly the "MultitouchPreferences" key. If we installed
     * "TrackpadUserPreferences" it would permanently shadow that live push -> "tapping always
     * clicks regardless of the checkbox". Seeding MultitouchPreferences activates gestures
     * pre-push, then gets overwritten/merged by the user's real settings -> the prefpane
     * controls actually take effect. */
    {
        OSDictionary *tp = OSDictionary::withCapacity(24);
        if (tp) {
            #define MT_SET_BOOL(k, v) do { tp->setObject(k, (v) ? kOSBooleanTrue : kOSBooleanFalse); } while (0)
            #define MT_SET_INT(k, v)  do { OSNumber *n = OSNumber::withNumber((unsigned long long)(v), 32); \
                if (n) { tp->setObject(k, n); n->release(); } } while (0)
            MT_SET_BOOL("Clicking", true);
            MT_SET_BOOL("Dragging", true);
            MT_SET_BOOL("TrackpadScroll", true);
            MT_SET_BOOL("TrackpadHorizScroll", true);
            MT_SET_BOOL("TrackpadMomentumScroll", true);
            MT_SET_BOOL("TrackpadPinch", true);
            MT_SET_BOOL("TrackpadRotate", true);
            MT_SET_BOOL("TrackpadRightClick", true);
            MT_SET_BOOL("TrackpadThreeFingerDrag", true);
            MT_SET_INT("TrackpadTwoFingerDoubleTapGesture", 1);
            MT_SET_INT("TrackpadThreeFingerTapGesture", 2);
            MT_SET_INT("TrackpadThreeFingerHorizSwipeGesture", 2);
            MT_SET_INT("TrackpadThreeFingerVertSwipeGesture", 2);
            MT_SET_INT("TrackpadFourFingerHorizSwipeGesture", 2);
            MT_SET_INT("TrackpadFourFingerVertSwipeGesture", 2);
            MT_SET_INT("TrackpadFourFingerPinchGesture", 2);
            MT_SET_INT("TrackpadFiveFingerPinchGesture", 2);
            MT_SET_INT("TrackpadTwoFingerFromRightEdgeSwipeGesture", 3);
            #undef MT_SET_BOOL
            #undef MT_SET_INT
            dev->setProperty("MultitouchPreferences", tp);
            tp->release();
            IOLog("mavericks_amd_terminal: MultitouchPreferences seeded\n");
        }
    }

    dev->registerService();
    IOLog("mavericks_amd_terminal: AppleMultitouchDevice started + registered\n");

    /* Retain the workloop NOW, after start() has established it.  Released LAST in teardown
     * (after the AMD itself is released) so nothing can call getWorkLoop() on freed memory —
     * the invariant our earlier panic violated. */
    ctx->wl = dev->getWorkLoop();
    if (ctx->wl) ctx->wl->retain();

    /* Raise the feed fence LAST: callers of mavericks_amd_terminal_amd() see NULL until the device
     * is fully built+started, and again once teardown clears it. */
    ctx->ready = true;

    return ctx;
}

AppleMultitouchDevice *mavericks_amd_terminal_amd(mavericks_amd_terminal_ctx *ctx) {
    return (ctx && ctx->ready) ? ctx->amd : 0;   /* no frame reaches a not-ready / tearing-down AMD */
}

/* ---- teardown ops (kext-side implementations of the host-tested order contract) -------------- */

static void op_clear_ready(void *c) {
    ((mavericks_amd_terminal_ctx *)c)->ready = false;    /* fence: amd() now returns NULL; no new frame enqueued */
}

static void op_term_shell(void *c) {
    mavericks_amd_terminal_ctx *ctx = (mavericks_amd_terminal_ctx *)c;
    if (ctx->shell) {
        ctx->shell->terminate();
        ctx->shell->release();
        ctx->shell = 0;
        IOLog("mavericks_amd_terminal: MT1 HID shell terminated + released\n");
    }
}

static void op_term_amd(void *c) {
    mavericks_amd_terminal_ctx *ctx = (mavericks_amd_terminal_ctx *)c;
    if (ctx->amd) {
        /* terminate(kIOServiceSynchronous) drives willTerminate->stop->detach itself so
         * Apple's frames-clients deregister before we release.  Do NOT also call
         * stop()/detach() — that was the direct-stop path that caused the getWorkLoop()
         * panic on freed memory. */
        ((IOService *)ctx->amd)->terminate(kIOServiceSynchronous);
        IOLog("mavericks_amd_terminal: AppleMultitouchDevice terminate(kIOServiceSynchronous) complete\n");
    }
}

static void op_release_amd(void *c) {
    mavericks_amd_terminal_ctx *ctx = (mavericks_amd_terminal_ctx *)c;
    if (ctx->amd) {
        ((IOService *)ctx->amd)->release();
        ctx->amd = 0;
        IOLog("mavericks_amd_terminal: AppleMultitouchDevice released\n");
    }
}

static void op_release_wl(void *c) {
    mavericks_amd_terminal_ctx *ctx = (mavericks_amd_terminal_ctx *)c;
    if (ctx->wl) {
        ctx->wl->release();
        ctx->wl = 0;
        IOLog("mavericks_amd_terminal: workloop released (last)\n");
    }
}

void mavericks_amd_terminal_teardown(IOService *nub, mavericks_amd_terminal_ctx *ctx) {
    (void)nub;
    if (!ctx) return;
    mavericks_synth_teardown_ops_t ops = {
        op_clear_ready, op_term_shell, op_term_amd, op_release_amd, op_release_wl, ctx
    };
    mavericks_synth_teardown_run(&ops);
    IODelete(ctx, mavericks_amd_terminal_ctx, 1);
    IOLog("mavericks_amd_terminal: teardown complete (ready-fenced, terminate()d, workloop released last)\n");
}

/* ---- fabricated-AMD terminal feed (one implementation; three consumers) --------------------- */

void mavericks_amd_terminal_feed(mavericks_amd_terminal_ctx *ctx, const MavericksTouchFrame *frame, uint32_t timestamp) {
    AppleMultitouchDevice *amd = mavericks_amd_terminal_amd(ctx);
    if (!amd) return;
    /* EDGE-CLAMP PROBE (debug.mt2_log>=2): per-frame decoded contact-0 x/y at the encode point. */
    if (frame->contact_count > 0)
        MT2_DLOG(2, "feed x=%d y=%d -> amd %p", frame->transducers[0].currentCoordinates.x,
                 frame->transducers[0].currentCoordinates.y, (void *)amd);
    unsigned char mt1[512];  /* 512 >= 256: safe for any realistic contact count */
    int n = mavericks_amd_construct_report(frame, mt1, sizeof mt1, timestamp);
    if (n > 0) amd->handleTouchFrame(mt1, (unsigned int)n);
}

void mavericks_amd_terminal_button(mavericks_amd_terminal_ctx *ctx, unsigned mask) {
    AppleMultitouchDevice *amd = mavericks_amd_terminal_amd(ctx);
    if (!amd) return;
    MT2_DLOG(2, "post_button_edge mask=0x%x -> amd %p", mask, (void *)amd);
    amd->handlePointerEventFromDevice(0, 0, mask, 0);
}

IOReturn mavericks_amd_terminal_inject(mavericks_amd_terminal_ctx *ctx, const unsigned char *bytes, unsigned int len) {
    AppleMultitouchDevice *amd = mavericks_amd_terminal_amd(ctx);
    if (!amd) return kIOReturnNotReady;
    return amd->handleTouchFrame((unsigned char *)bytes, len);
}
