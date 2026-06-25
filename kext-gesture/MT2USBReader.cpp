/* MT2USBReader — in-kernel USB transport for the Magic Trackpad 2.
 *
 * Matches interface 1 of the cabled MT2 at a high probe score (beating IOUSBHIDDriver),
 * then manual-starts Apple's genuine AppleUSBMultitouchDriver on the interface and
 * interposes its handleReport via an instance-scoped vtable clone, so each MT2 0x02
 * report is reframed into the CompactV4 packet Apple's recognizer accepts
 * (mt2_usb_to_compactv4). The genuine driver owns the interface + interrupt pipe and
 * does all the contact/gesture/cursor work; we only translate the stream at its seam.
 *
 * Replaces the inert MT2USBClaim kext plus the userspace feeder loop.
 */
#include <IOKit/IOLib.h>
#include <IOKit/hid/IOHIDDevice.h>     /* IOHIDReportType */
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>                /* clock_get_system_microtime */
#include "MT2USBReader.h"
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "vtable_clone.h"              /* instance-scoped vtable clone/override/restore */
#include "mt2_usb_reframe.h"           /* mt2_usb_to_compactv4 (host-tested reframe) */
#include "../src/mt2_coordinator.h"    /* transport-coordinator seam (no-op for MT2) */
#include "gh_default_adapter.h"        /* shared generic alloc/class_ok/start/detach/terminate/release */

/* handleReport vtable BYTE offset 0x8b8 -> slot index (RE'd 2026-06-24). */
#define USB_HANDLEREPORT_SLOT_INDEX  (0x8b8 / sizeof(void *))
/* handleButton vtable BYTE offset 0xb28 -> slot index (RE'd 2026-06-24). The genuine driver gets the
 * physical button from a separate button-provider service (buttonPublished) that our manual-start
 * lacks, so handleButton never fires; we call it ourselves with a button report (byte[15]=state). */
#define USB_HANDLEBUTTON_SLOT_INDEX  (0xb28 / sizeof(void *))
#define USB_VTABLE_SPAN              0x2000

static vtc_clone_t gUsbVtableClone;
static bool        gUsbVtableCloned = false;
typedef IOReturn (*usb_handle_report_fn)(void *self, IOMemoryDescriptor *report,
                                         IOHIDReportType type, IOOptionBits options);
static usb_handle_report_fn gOrigUsbHandleReport = 0;
typedef void (*usb_handle_button_fn)(void *self, uint8_t *report);
static uint8_t gLastUsbButton = 0;             /* last physical-button state seen (edge-gated) */

/* Post-liftoff absence-frame pump. The genuine recognizer's deferred tap commits (e.g.
 * MTTapDragManager::sendPendingSecondaryTap, the 2-finger TAP secondary click) run once PER FRAME and
 * key off the frame timestamp; our device goes silent at liftoff, so an isolated tap's commit window
 * starves. After the device falls silent we pump zero-contact frames (advancing wall-clock ts) for a
 * window long enough to cover the double-tap commit window. A new real report re-arms the silence
 * timer, so we never pump while a gesture is active. (RE'd 2026-06-24; mirrors the synthetic path's
 * emit_with_liftoff absence pump, extended for the longer secondary-tap window.) */
#define USB_PUMP_SILENCE_MS   20               /* fire this long after the last real report */
#define USB_PUMP_INTERVAL_MS  15               /* spacing between pumped absence frames */
#define USB_PUMP_FRAMES       30               /* ~30 * 15ms = ~450ms window */
static IOWorkLoop          *gUsbWorkLoop = 0;
static IOTimerEventSource  *gPumpTimer   = 0;
static void                *gGenuineSelf = 0;  /* the genuine driver instance (handleReport target) */
static volatile int         gPumpBudget  = 0;  /* absence frames left to pump after silence */

static uint32_t usb_ts_22bit(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint32_t)(((uint64_t)s * 1000 + u / 1000) & 0x3FFFFF);   /* monotonic ms, 22-bit */
}

/* Instance-scoped override of the genuine driver's handleReport: read the raw MT2 0x02 report,
 * reframe to Apple's CompactV4 packet (mt2_usb_to_compactv4), wrap in a fresh descriptor, and chain
 * the original. Non-touch reports pass through untouched. extern "C" free fn; first arg is `this`. */
extern "C" IOReturn mt2_usb_handle_report(void *self, IOMemoryDescriptor *report,
                                          IOHIDReportType type, IOOptionBits options) {
    if (!gOrigUsbHandleReport) return kIOReturnError;
    if (!report) return gOrigUsbHandleReport(self, report, type, options);

    uint8_t mt2[256], out[256];
    IOByteCount mn = report->getLength();
    if (mn == 0 || mn > sizeof(mt2)) return gOrigUsbHandleReport(self, report, type, options);
    report->readBytes(0, mt2, mn);
    if (mt2[0] != 0x02)                                   /* not a touch report: pass through untouched */
        return gOrigUsbHandleReport(self, report, type, options);

    /* Physical click: the genuine driver's handleButton is normally fed by a button-provider service
     * our manual-start lacks, so it never fires. Detect the button edge in the report and drive
     * handleButton ourselves (reuses Apple's dispatch to the already-wired event driver). */
    uint8_t btnrep[16];
    if (mt2_usb_button_edge(mt2, (size_t)mn, &gLastUsbButton, btnrep)) {
        usb_handle_button_fn hb = (usb_handle_button_fn)((*(void ***)self)[USB_HANDLEBUTTON_SLOT_INDEX]);
        if (hb) hb(self, btnrep);
    }

    size_t outlen = 0;
    if (mt2_usb_to_compactv4(mt2, (size_t)mn, usb_ts_22bit(), out, sizeof(out), &outlen) != 0)
        return gOrigUsbHandleReport(self, report, type, options);

    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withCapacity(outlen, kIODirectionIn);
    if (!md) return kIOReturnNoMemory;
    md->setLength(outlen);
    md->writeBytes(0, out, outlen);
    IOReturn rc = gOrigUsbHandleReport(self, md, type, options);
    md->release();

    /* A real report just arrived: refill the pump budget and (re)arm the silence timer, so the
     * post-liftoff absence pump fires only after the device falls quiet — never during a gesture. */
    gPumpBudget = USB_PUMP_FRAMES;
    if (gPumpTimer) gPumpTimer->setTimeoutMS(USB_PUMP_SILENCE_MS);
    return rc;
}

/* Timer callback (workloop): the device has been silent for USB_PUMP_SILENCE_MS. Emit one zero-contact
 * absence frame into the genuine driver's handleReport so the recognizer's deferred per-frame commit
 * checks keep running through the double-tap window, then re-arm until the budget is spent. */
static void mt2_usb_pump_action(OSObject * /*owner*/, IOTimerEventSource *ts) {
    if (gPumpBudget <= 0 || !gOrigUsbHandleReport || !gGenuineSelf) return;
    uint8_t out[64]; size_t outlen = 0;
    if (mt2_usb_make_absence_frame(usb_ts_22bit(), out, sizeof(out), &outlen) == 0) {
        IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withCapacity(outlen, kIODirectionIn);
        if (md) {
            md->setLength(outlen);
            md->writeBytes(0, out, outlen);
            gOrigUsbHandleReport(gGenuineSelf, md, kIOHIDReportTypeInput, 0);
            md->release();
        }
    }
    if (--gPumpBudget > 0 && ts) ts->setTimeoutMS(USB_PUMP_INTERVAL_MS);
}

OSDefineMetaClassAndStructors(com_schmonz_MT2USBReader, IOService)

bool com_schmonz_MT2USBReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fIntf = OSDynamicCast(IOUSBInterface, provider);
    if (!fIntf) { IOLog("MT2USBReader: provider not IOUSBInterface\n"); return false; }
    fGenuine = 0;

    /* Manual-start Apple's AppleUSBMultitouchDriver on our interface and interpose its handleReport
     * (the CompactV4 reframe seam, see startGenuine). The genuine driver owns the interface + the
     * interrupt pipe; we never open them ourselves. */
    return startGenuine(provider);
}

/* Set a 32-bit integer property in an init dictionary (released after init copies it). */
static void mt2_dict_num(OSDictionary *d, const char *key, uint32_t val) {
    OSNumber *n = OSNumber::withNumber(val, 32);
    if (n) { d->setObject(key, n); n->release(); }
}

/* Set a C-string property in an init dictionary (released after init copies it). */
static void mt2_dict_str(OSDictionary *d, const char *key, const char *val) {
    OSString *s = OSString::withCString(val);
    if (s) { d->setObject(key, s); s->release(); }
}

/* Build the init dict that seeds the manually-started AppleUSBMultitouchDriver. Seed via the INIT
 * dictionary, not setProperty: the driver overrides every setProperty variant and drops unknown keys,
 * but init() forwards the dict to super::init which populates the property table directly. Manual-start
 * does no personality merge AND the device NAKs Apple's feature reports, so without these the instance
 * lacks its user-client class (-> 0 frames clients), sensor geometry, and the hidd-engagement props
 * (HIDServiceSupport + IOCFPlugInTypes -> MultitouchHID.plugin). HIDDefaultBehavior=Trackpad (NOT the
 * personality's Mouse) because MT2 multitouch-streaming emits no mouse reports. parser-options 39=0x27
 * (bit 0x2 = clicky-hardware gate); prefs seeded under both keys so click/right-click enable. Values
 * from src/mt2_geometry.c + Apple's genuine USB personality (RE'd 2026-06-24). Caller releases. */
static OSDictionary *usb_build_init_props(void) {
    OSDictionary *initp = OSDictionary::withCapacity(24);
    if (!initp) return 0;
    OSString *ucc = OSString::withCString("AppleUSBMultitouchUserClient");
    if (ucc) { initp->setObject("IOUserClientClass", ucc); ucc->release(); }
    mt2_dict_num(initp, "Family ID", 0x80);            /* 128 */
    mt2_dict_num(initp, "Sensor Rows", 13);
    mt2_dict_num(initp, "Sensor Columns", 16);
    mt2_dict_num(initp, "Sensor Surface Width", 13000);
    mt2_dict_num(initp, "Sensor Surface Height", 11300);
    mt2_dict_num(initp, "parser-type", 1000);
    mt2_dict_num(initp, "parser-options", 39);
    initp->setObject("Driver is Ready", kOSBooleanTrue);
    initp->setObject("MTHIDDevice", kOSBooleanTrue);
    mt2_dict_str(initp, "Product", "Magic Trackpad 2");
    initp->setObject("HIDServiceSupport", kOSBooleanTrue);
    mt2_dict_str(initp, "HIDDefaultBehavior", "Trackpad");
    initp->setObject("TrackpadMomentumScroll", kOSBooleanTrue);
    initp->setObject("TrackpadFourFingerGestures", kOSBooleanTrue);
    initp->setObject("TrackpadSecondaryClickCorners", kOSBooleanTrue);
    initp->setObject("TrackpadThreeFingerDrag", kOSBooleanTrue);
    initp->setObject("ExtractAndPostDeviceButtonState", kOSBooleanTrue);
    const char *prefKeys[] = { "Clicking", "Dragging", "TrackpadRightClick",
        "TrackpadSecondaryClickCorners", "TrackpadMomentumScroll", "TrackpadScroll",
        "TrackpadThreeFingerDrag", "TrackpadFourFingerGestures" };
    OSDictionary *prefs = OSDictionary::withCapacity(8);
    if (prefs) {
        for (unsigned i = 0; i < sizeof(prefKeys) / sizeof(prefKeys[0]); i++)
            prefs->setObject(prefKeys[i], kOSBooleanTrue);
        initp->setObject("MultitouchPreferences", prefs);
        initp->setObject("TrackpadUserPreferences", prefs);
        prefs->release();
    }
    OSDictionary *plugins = OSDictionary::withCapacity(1);
    OSString *pluginPath = OSString::withCString(
        "AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin");
    if (plugins && pluginPath) {
        plugins->setObject("0516B563-B15B-11DA-96EB-0014519758EF", pluginPath);
        initp->setObject("IOCFPlugInTypes", plugins);
    }
    if (pluginPath) pluginPath->release();
    if (plugins) plugins->release();
    return initp;
}

/* ---- genuine_host adapter: the six generic ops are the shared gh_default_* (gh_default_adapter.h);
 * USB supplies only init_attach (seed dict + attach), interpose (handleReport vtable clone), restore.
 * These three read only h->obj/h->provider (+ file-static clone state), so they need no reader ctx. ---- */
static bool usb_gh_init_attach(gh_host_t *h) {
    OSDictionary *initp = usb_build_init_props();
    bool ok = initp && ((IOService *)h->obj)->init(initp)
                    && ((IOService *)h->obj)->attach((IOService *)h->provider);
    if (initp) initp->release();
    return ok;
}
static int usb_gh_interpose(gh_host_t *h) {
    if (vtc_clone_override(h->obj, USB_VTABLE_SPAN, USB_HANDLEREPORT_SLOT_INDEX,
                           (void *)&mt2_usb_handle_report, &gUsbVtableClone) != 0) return -1;
    gOrigUsbHandleReport = (usb_handle_report_fn)
        (((void **)gUsbVtableClone.orig_vptr)[USB_HANDLEREPORT_SLOT_INDEX]);
    gUsbVtableCloned = true;
    return 0;
}
static void usb_gh_restore(gh_host_t *h) {
    if (gUsbVtableCloned) {
        vtc_restore(h->obj, &gUsbVtableClone);
        gUsbVtableCloned = false; gOrigUsbHandleReport = 0;
    }
}
static const gh_adapter_t kUsbAdapter = {
    gh_default_alloc, gh_default_class_ok, usb_gh_init_attach, usb_gh_interpose,
    gh_default_start, usb_gh_restore, gh_default_detach, gh_default_terminate, gh_default_release
};

/* Genuine-USB path: host a genuine AppleUSBMultitouchDriver on our interface via the shared
 * genuine_host core (manual-start + class-gate + handleReport vtable interpose + start), then send the
 * MT2 USB multitouch-enable and arm the absence pump. The genuine driver OWNS the interface + interrupt
 * pipe; our reader must NOT open them. gh_start fully unwinds on any failure. */
bool com_schmonz_MT2USBReader::startGenuine(IOService *provider) {
    (void)provider;
    mt2_usb_reframe_reset();          /* fresh per-finger lifecycle history for this stream */
    gLastUsbButton = 0;               /* fresh physical-button edge state for this stream */

    static const gh_config_t cfg = { "AppleUSBMultitouchDriver", "AppleUSBMultitouchDriver" };
    if (gh_start(&fHost, &cfg, &kUsbAdapter, this, fIntf) != 0) {
        IOLog("MT2USBReader: genuine host start failed\n");
        return false;
    }
    fGenuine = (IOService *)fHost.obj;
    (void)mt2_coordinator_activate(MT2_XPORT_USB, 0);   /* no-op seam (MT2 single-transport) */

    /* The genuine driver doesn't send MT2's USB multitouch-enable; we do. */
    IOUSBDevRequest en;
    uint8_t payload[2] = {0x02, 0x01};
    en.bmRequestType = 0x21; en.bRequest = 0x09;
    en.wValue = 0x0302; en.wIndex = 1; en.wLength = sizeof(payload); en.pData = payload;
    IOReturn enrc = fIntf->DeviceRequest(&en);
    IOLog("MT2USBReader: multitouch-enable SET_REPORT rc=0x%08x\n", enrc);

    /* Post-liftoff absence-frame pump: a workloop timer that keeps a brief frame heartbeat alive after
     * the device falls silent, so deferred tap commits (2-finger TAP secondary click) fire instead of
     * starving. Best-effort — if setup fails, the rest still works. */
    gGenuineSelf = fGenuine;
    gPumpBudget  = 0;
    gUsbWorkLoop = IOWorkLoop::workLoop();
    if (gUsbWorkLoop) {
        gPumpTimer = IOTimerEventSource::timerEventSource(this, &mt2_usb_pump_action);
        if (gPumpTimer && gUsbWorkLoop->addEventSource(gPumpTimer) != kIOReturnSuccess) {
            gPumpTimer->release(); gPumpTimer = 0;
        }
    }

    IOLog("MT2USBReader: genuine AppleUSBMultitouchDriver started + handleReport interposed (slot %lu)\n",
          (unsigned long)USB_HANDLEREPORT_SLOT_INDEX);
    registerService();
    return true;
}

/* Relinquish our hold on the interface so the provider subtree can finalize. MUST run during the
 * willTerminate handshake (device unplug / re-enumerate): IOKit does not call stop() until the
 * interface is released, so deferring teardown to stop() deadlocks it — the reader stays
 * inactive/busy and pins the whole dead device subtree (leaked, never freed; one per unplug).
 * Idempotent. Reverses startGenuine: the genuine driver owns the interface + interrupt pipe, so we
 * never opened fIntf ourselves and must NOT close it (an unmatched close would unbalance the open
 * count) — we only null it. */
void com_schmonz_MT2USBReader::releaseInterface(void) {
    /* Stop the absence pump FIRST: cancel + remove from the workloop so no pumped frame can race the
     * teardown below (it touches gGenuineSelf / gOrigUsbHandleReport). Then drop the workloop. */
    if (gPumpTimer) {
        gPumpTimer->cancelTimeout();
        if (gUsbWorkLoop) gUsbWorkLoop->removeEventSource(gPumpTimer);
        gPumpTimer->release(); gPumpTimer = 0;
    }
    if (gUsbWorkLoop) { gUsbWorkLoop->release(); gUsbWorkLoop = 0; }
    gPumpBudget = 0; gGenuineSelf = 0;

    /* genuine_host ordered, idempotent teardown: restore the vtable FIRST (avoids the in-flight
     * handleReport use-after-free), then terminate + release the genuine driver. State-aware + safe to
     * call twice (willTerminate then stop). */
    gh_stop(&fHost, &kUsbAdapter);
    fGenuine = 0;
    fIntf = 0;                       /* we never opened it; don't close */
}

bool com_schmonz_MT2USBReader::willTerminate(IOService *provider, IOOptionBits options) {
    releaseInterface();
    return IOService::willTerminate(provider, options);
}

void com_schmonz_MT2USBReader::stop(IOService *provider) {
    releaseInterface();                          /* no-op if willTerminate already did it */
    IOLog("MT2USBReader: stopped\n");
    IOService::stop(provider);
}
