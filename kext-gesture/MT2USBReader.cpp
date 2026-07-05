/* MT2USBReader — the USB transport reader for the Magic Trackpad 2.
 *
 * Two halves. PER-TRANSPORT (the ~3%): match interface 1 of the cabled MT2, manual-start
 * Apple's genuine AppleUSBMultitouchDriver on it, seed the config it needs (geometry +
 * user-client, via the init dict), and send the MT2 0x02 USB multitouch-enable. SHARED
 * ENGINE (the ~97%) is what the seam feeds. There is ONE seam: mt2_usb_handle_report runs
 * in place of the genuine driver's handleReport, reframes each raw MT2 0x02 report into the
 * CompactV4 packet Apple's recognizer accepts (mt2_usb_to_compactv4), and chains the
 * original. Apple's driver owns the interface + interrupt pipe and does all the
 * contact/gesture/cursor work; we only translate the stream at its seam.
 *
 * Dirty tricks, named where used: mt2_usb_handle_report (THE SEAM, the handleReport splice),
 * usb_gh_interpose (instance-scoped vtable clone), the pre-start 0x02 enable + settle, the
 * self-driven handleButton. Load-bearing RE prose is in docs/mt-stack/explanation.md
 * "MT2USBReader bring-up"; the code keeps one-line pointers.
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
#include "mt2_usb_decode.h"            /* mt2_usb_decode -> VoodooInputEvent (the decode seam) */
#include "mt2_usb_reframe.h"           /* usb_assemble_compactv4 + button-edge/absence helpers */
#include "../src/mt2_coordinator.h"    /* transport-coordinator seam (no-op for MT2) */
#include "gh_default_adapter.h"        /* shared generic alloc/class_ok/start/detach/terminate/release */
#include "mt2_geometry.h"              /* MT2_SURFACE_WIDTH/HEIGHT — one knob shared with the BT report */

/* handleReport vtable BYTE offset 0x8b8 -> slot index (RE'd 2026-06-24). */
#define USB_HANDLEREPORT_SLOT_INDEX  (0x8b8 / sizeof(void *))
/* handleButton vtable BYTE offset 0xb28 -> slot index (RE'd 2026-06-24). The genuine driver gets the
 * physical button from a separate button-provider service (buttonPublished) that our manual-start
 * lacks, so handleButton never fires; we call it ourselves with a button report (byte[15]=state). */
#define USB_HANDLEBUTTON_SLOT_INDEX  (0xb28 / sizeof(void *))
#define USB_VTABLE_SPAN              0x2000
/* Settle after the enable, before starting the AMD, or a mouse-mode getReport storm can panic —
 * see explanation.md "MT2USBReader bring-up". 50ms measured sufficient; don't delete it. */
#define MT2_USB_ENABLE_SETTLE_MS     50

static vtc_clone_t gUsbVtableClone;
static bool        gUsbVtableCloned = false;
typedef IOReturn (*usb_handle_report_fn)(void *self, IOMemoryDescriptor *report,
                                         IOHIDReportType type, IOOptionBits options);
static usb_handle_report_fn gOrigUsbHandleReport = 0;
typedef void (*usb_handle_button_fn)(void *self, uint8_t *report);
static uint8_t gLastUsbButton = 0;             /* last physical-button state seen (edge-gated) */

/* Post-liftoff absence-frame pump: after the device falls silent at liftoff we emit zero-contact
 * frames so the recognizer's deferred per-frame tap commits (2-finger secondary click) don't starve.
 * A real report re-arms the silence timer, so we never pump during a gesture — see explanation.md
 * "MT2USBReader bring-up". */
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

/* THE SEAM (the reframe splice): runs in place of the genuine driver's handleReport (usb_gh_interpose
 * points the vtable slot here). Read the raw MT2 0x02 report, then — structurally mirroring the BT
 * reader (mt2_bt_decode -> submitFrame) — DECODE to the VoodooInputEvent seam (mt2_usb_decode) and run
 * the USB ASSEMBLY on it (usb_assemble_compactv4: drop-lifted + lifecycle + mt1_encode + checksum),
 * wrap in a fresh descriptor, and chain the original; non-touch reports pass through untouched.
 * extern "C" free fn; first arg is `this`. */
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

    /* Dirty trick (self-driven handleButton): the genuine driver's handleButton is normally fed by a
     * button-provider service our manual-start lacks, so it never fires. Detect the button edge in the
     * report and drive handleButton ourselves (reuses Apple's dispatch to the already-wired driver). */
    uint8_t btnrep[16];
    if (mt2_usb_button_edge(mt2, (size_t)mn, &gLastUsbButton, btnrep)) {
        usb_handle_button_fn hb = (usb_handle_button_fn)((*(void ***)self)[USB_HANDLEBUTTON_SLOT_INDEX]);
        if (hb) hb(self, btnrep);
    }

    VoodooInputEvent frame;
    if (mt2_usb_decode(mt2, (size_t)mn, &frame) != 0)          /* decode -> VoodooInputEvent (seam) */
        return gOrigUsbHandleReport(self, report, type, options);
    size_t outlen = 0;
    if (usb_assemble_compactv4(&frame, usb_ts_22bit(), out, sizeof(out), &outlen) != 0)  /* assembly */
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

    /* Manual-start Apple's genuine AppleUSBMultitouchDriver on our interface and interpose its
     * handleReport (the CompactV4 reframe seam) — startGenuine reads as named steps. The genuine
     * driver owns the interface + the interrupt pipe; we never open them ourselves. */
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

/* Set a raw-bytes (OSData) property in an init dictionary (released after init copies it). */
static void mt2_dict_data(OSDictionary *d, const char *key, const void *bytes, unsigned int len) {
    OSData *o = OSData::withBytes(bytes, len);
    if (o) { d->setObject(key, o); o->release(); }
}

/* Build the init dict that seeds the manually-started AppleUSBMultitouchDriver. Seed via the INIT
 * dict, NOT setProperty (the driver drops unknown setProperty keys; init forwards to super::init which
 * populates the table). Manual-start does no personality merge and the device NAKs Apple's feature
 * reports, so without these the instance lacks its user-client, geometry, and hidd-engagement props —
 * see explanation.md "MT2USBReader bring-up". Values from src/mt2_geometry.c + Apple's genuine USB
 * personality. Caller releases; returned as void* (the gh_config_t build_props signature). */
static void *usb_build_init_props(void) {
    OSDictionary *initp = OSDictionary::withCapacity(24);
    if (!initp) return 0;
    OSString *ucc = OSString::withCString("AppleUSBMultitouchUserClient");
    if (ucc) { initp->setObject("IOUserClientClass", ucc); ucc->release(); }
    /* GENUINE MT2 geometry (one source of truth in mt2_geometry.h). */
    mt2_dict_num(initp, "Family ID", MT2_FAMILY_ID);   /* 129 */
    mt2_dict_num(initp, "Sensor Rows", MT2_SENSOR_ROWS);     /* 22 */
    mt2_dict_num(initp, "Sensor Columns", MT2_SENSOR_COLS);  /* 30 */
    mt2_dict_num(initp, "Sensor Surface Width", MT2_SURFACE_WIDTH);    /* 15600 = 156mm */
    mt2_dict_num(initp, "Sensor Surface Height", MT2_SURFACE_HEIGHT);  /* 11040 = 110.4mm */
    mt2_dict_data(initp, "Sensor Region Descriptor", mt2_region_descriptor, sizeof(mt2_region_descriptor));
    mt2_dict_data(initp, "Sensor Region Param", mt2_region_param, sizeof(mt2_region_param));
    {   /* Surface Descriptor = Width/Height u32 LE + the genuine 8-byte tail */
        unsigned char sdesc[16];
        sdesc[0] = MT2_SURFACE_WIDTH & 0xff;        sdesc[1] = (MT2_SURFACE_WIDTH >> 8) & 0xff;
        sdesc[2] = (MT2_SURFACE_WIDTH >> 16) & 0xff; sdesc[3] = (MT2_SURFACE_WIDTH >> 24) & 0xff;
        sdesc[4] = MT2_SURFACE_HEIGHT & 0xff;       sdesc[5] = (MT2_SURFACE_HEIGHT >> 8) & 0xff;
        sdesc[6] = (MT2_SURFACE_HEIGHT >> 16) & 0xff; sdesc[7] = (MT2_SURFACE_HEIGHT >> 24) & 0xff;
        for (unsigned i = 0; i < 8; i++) sdesc[8 + i] = mt2_surface_desc_tail[i];
        mt2_dict_data(initp, "Sensor Surface Descriptor", sdesc, sizeof(sdesc));
    }
    mt2_dict_num(initp, "parser-type", 1000);
    mt2_dict_num(initp, "parser-options", 39);   /* 0x27, bit 0x2 = clicky-hardware gate */
    initp->setObject("Driver is Ready", kOSBooleanTrue);
    initp->setObject("MTHIDDevice", kOSBooleanTrue);
    /* No "Product" seed: the genuine AMD's start copies the device's real USB iProduct descriptor
     * ("Magic Trackpad") onto the node, overriding any seed (verified live). See explanation.md. */
    initp->setObject("HIDServiceSupport", kOSBooleanTrue);
    mt2_dict_str(initp, "HIDDefaultBehavior", "Trackpad");   /* NOT the personality's Mouse: MT2 streams no mouse reports */
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

/* genuine_host adapter: the seven generic ops are the shared gh_default_* (gh_default_adapter.h), with
 * usb_build_init_props supplied via cfg.build_props; USB supplies only interpose (the handleReport
 * vtable clone) + restore. These read only h->obj (+ file-static clone state), so they need no ctx. */
/* Dirty trick (instance-scoped vtable clone): clone the genuine driver's vtable on OUR instance and
 * point the handleReport slot at mt2_usb_handle_report (THE SEAM). Instance-scoped, so only this
 * driver's vtable pointer changes; the shared class vtable is untouched. Saves the original for the shim. */
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
    gh_default_alloc, gh_default_class_ok, gh_default_init_attach, usb_gh_interpose,
    gh_default_start, usb_gh_restore, gh_default_detach, gh_default_terminate, gh_default_release
};

/* Genuine-USB path, as a sequence of named steps (named alike to MT2BTReader where the action matches):
 * reset per-stream state, send the MT2 multitouch-enable, settle, host + interpose the genuine driver,
 * arm the absence pump. The enable→settle→start ORDER is load-bearing (panic hardening) — preserve it.
 * The genuine driver OWNS the interface + interrupt pipe; our reader must NOT open them. */
bool com_schmonz_MT2USBReader::startGenuine(IOService *provider) {
    (void)provider;

    resetTransportState();          /* fresh per-stream reframe + button-edge state */
    sendEnable();                   /* MT2 USB multitouch-enable (control transfer on fIntf) */
    settle();                       /* device leaves mouse mode before the AMD probes it */
    if (!manualStartGenuineAmd())   /* host the genuine AMD + interpose handleReport */
        return false;
    armAbsencePump();               /* post-liftoff absence-frame heartbeat (deferred tap commits) */

    IOLog("MT2USBReader: genuine AppleUSBMultitouchDriver started + handleReport interposed (slot %lu)\n",
          (unsigned long)USB_HANDLEREPORT_SLOT_INDEX);
    registerService();
    return true;
}

/* Step: zero the per-stream state (extracted from startGenuine). */
void com_schmonz_MT2USBReader::resetTransportState() {
    mt2_usb_reframe_reset();   /* fresh per-finger lifecycle history for this stream */
    gLastUsbButton = 0;        /* fresh physical-button edge state for this stream */
}

/* Step: send the MT2's USB multitouch-enable (SET_REPORT control transfer on fIntf). Sent BEFORE
 * starting the AMD — the enable is independent of the AMD and the ordering is load-bearing (panic
 * hardening) — see explanation.md "MT2USBReader bring-up". */
void com_schmonz_MT2USBReader::sendEnable() {
    IOUSBDevRequest en;
    uint8_t payload[2] = {0x02, 0x01};
    en.bmRequestType = 0x21; en.bRequest = 0x09;
    en.wValue = 0x0302; en.wIndex = 1; en.wLength = sizeof(payload); en.pData = payload;
    IOReturn enrc = fIntf->DeviceRequest(&en);
    IOLog("MT2USBReader: multitouch-enable SET_REPORT rc=0x%08x (pre-start)\n", enrc);
}

/* Step: let the device's async mode switch finish after the enable, before the AMD probes it, or a
 * mouse-mode getReport storm can panic — see explanation.md "MT2USBReader bring-up". */
void com_schmonz_MT2USBReader::settle() {
    IOSleep(MT2_USB_ENABLE_SETTLE_MS);
}

/* Step: host a genuine AppleUSBMultitouchDriver on our interface via the shared genuine_host core
 * (manual-start + class-gate + handleReport vtable interpose + start; gh_start fully unwinds on
 * failure). Sets fGenuine on success. */
bool com_schmonz_MT2USBReader::manualStartGenuineAmd() {
    static const gh_config_t cfg = { "AppleUSBMultitouchDriver", "AppleUSBMultitouchDriver",
                                     usb_build_init_props };
    if (gh_start(&fHost, &cfg, &kUsbAdapter, this, fIntf) != 0) {
        IOLog("MT2USBReader: genuine host start failed\n");
        return false;
    }
    fGenuine = (IOService *)fHost.obj;
    (void)mt2_coordinator_activate(MT2_XPORT_USB, 0);   /* no-op seam (MT2 single-transport) */
    return true;
}

/* Step: arm the post-liftoff absence-frame pump — a workloop timer that keeps a brief frame heartbeat
 * alive after the device falls silent, so deferred tap commits (2-finger TAP secondary click) fire
 * instead of starving. Best-effort — if setup fails, the rest still works. */
void com_schmonz_MT2USBReader::armAbsencePump() {
    gGenuineSelf = fGenuine;
    gPumpBudget  = 0;
    gUsbWorkLoop = IOWorkLoop::workLoop();
    if (gUsbWorkLoop) {
        gPumpTimer = IOTimerEventSource::timerEventSource(this, &mt2_usb_pump_action);
        if (gPumpTimer && gUsbWorkLoop->addEventSource(gPumpTimer) != kIOReturnSuccess) {
            gPumpTimer->release(); gPumpTimer = 0;
        }
    }
}

/* Reverse startGenuine; idempotent. MUST run during willTerminate (device unplug / re-enumerate): IOKit
 * defers stop() until the interface is released, so a stop()-only teardown deadlocks and leaks the dead
 * device subtree — see explanation.md "MT2USBReader bring-up". We never opened fIntf (the genuine driver
 * did), so we only null it, never close it. */
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
