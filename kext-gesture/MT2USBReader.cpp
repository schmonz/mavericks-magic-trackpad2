/* MT2USBReader — in-kernel USB interrupt transport for the Magic Trackpad 2.
 *
 * A thin decoder, the USB sibling of MT2BTReader. Matches interface 1 of the
 * cabled MT2 at a high probe score (beating IOUSBHIDDriver), opens the
 * interrupt-IN pipe, sends the SET_REPORT enable-multitouch control request
 * (the same {0x02,0x01} payload the old userspace mt2_usb_read.c used), and
 * async-reads frames. Each frame is decoded (mt2_usb_decode) and pushed to the
 * MT2Gesture nub via submitFrame(MT2_STREAMING). The shared mt2_session owns the
 * settle gate and all post-decode logic — there is no decision logic here.
 *
 * Replaces the inert MT2USBClaim kext plus the userspace feeder loop.
 */
#include <IOKit/IOLib.h>
#include <IOKit/hid/IOHIDDevice.h>     /* IOHIDReportType */
#include <kern/clock.h>                /* clock_get_system_microtime */
#include "MT2USBReader.h"
#include "MT2Gesture.h"
#include "mt2_pipeline.h"
#include "mt2_usb_decode.h"
#define VTC_ALLOC(sz)  IOMalloc(sz)
#define VTC_FREE(p,sz) IOFree((p), (sz))
#include "vtable_clone.h"              /* instance-scoped vtable clone/override/restore */
#include "mt2_usb_reframe.h"           /* mt2_usb_to_compactv4 (host-tested reframe) */
#include "mt2_build_flags.h"           /* kGenuineUsb */

/* Set by com_schmonz_MT2Gesture::start (same kext). */
extern com_schmonz_MT2Gesture *gActiveMT2Gesture;

/* handleReport vtable BYTE offset 0x8b8 -> slot index (RE'd 2026-06-24). */
#define USB_HANDLEREPORT_SLOT_INDEX  (0x8b8 / sizeof(void *))
#define USB_VTABLE_SPAN              0x2000

static vtc_clone_t gUsbVtableClone;
static bool        gUsbVtableCloned = false;
typedef IOReturn (*usb_handle_report_fn)(void *self, IOMemoryDescriptor *report,
                                         IOHIDReportType type, IOOptionBits options);
static usb_handle_report_fn gOrigUsbHandleReport = 0;

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

    size_t outlen = 0;
    if (mt2_usb_to_compactv4(mt2, (size_t)mn, usb_ts_22bit(), out, sizeof(out), &outlen) != 0)
        return gOrigUsbHandleReport(self, report, type, options);

    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withCapacity(outlen, kIODirectionIn);
    if (!md) return kIOReturnNoMemory;
    md->setLength(outlen);
    md->writeBytes(0, out, outlen);
    IOReturn rc = gOrigUsbHandleReport(self, md, type, options);
    md->release();
    return rc;
}

OSDefineMetaClassAndStructors(com_schmonz_MT2USBReader, IOService)

bool com_schmonz_MT2USBReader::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fIntf = OSDynamicCast(IOUSBInterface, provider);
    if (!fIntf) { IOLog("MT2USBReader: provider not IOUSBInterface\n"); return false; }
    fStopping = false; fPipe = 0; fMaxPacket = 0; fBuf = 0; fGenuine = 0;

    /* Genuine-USB path (flag-gated, default false): let Apple's AppleUSBMultitouchDriver own the
     * interface and interpose its handleReport. The synthetic body below does NOT run in this mode. */
    if (kGenuineUsb) return startGenuine(provider);

    if (!fIntf->open(this)) { IOLog("MT2USBReader: open failed\n"); return false; }

    IOUSBFindEndpointRequest req;
    req.type = kUSBInterrupt; req.direction = kUSBIn;
    req.maxPacketSize = 0; req.interval = 0;
    fPipe = fIntf->FindNextPipe(0, &req);
    if (!fPipe) { IOLog("MT2USBReader: no interrupt-IN pipe\n"); fIntf->close(this); return false; }
    fMaxPacket = fPipe->GetMaxPacketSize();

    /* enable multitouch: SET_REPORT feature, report id 0x02, payload {0x02,0x01}
       (identical to mt2_usb_read.c). */
    IOUSBDevRequest en;
    uint8_t payload[2] = {0x02, 0x01};
    en.bmRequestType = 0x21; en.bRequest = 0x09;
    en.wValue = 0x0302; en.wIndex = 1; en.wLength = sizeof(payload); en.pData = payload;
    fIntf->DeviceRequest(&en);

    fBuf = IOBufferMemoryDescriptor::withCapacity(256, kIODirectionIn);
    if (!fBuf) { fIntf->close(this); return false; }

    if (gActiveMT2Gesture) gActiveMT2Gesture->connectionEstablished(this, MT2_STREAMING);
    IOLog("MT2USBReader: interface open, multitouch enabled, reading (mps=%u)\n",
          (unsigned)fMaxPacket);
    registerService();
    armRead();
    return true;
}

void com_schmonz_MT2USBReader::armRead(void) {
    if (fStopping || !fPipe || !fBuf) return;
    IOUSBCompletion c;
    c.target = this; c.action = &com_schmonz_MT2USBReader::readComplete; c.parameter = 0;
    fBuf->setLength(fMaxPacket ? fMaxPacket : 64);
    IOReturn r = fPipe->Read(fBuf, 0, 0, &c);
    if (r != kIOReturnSuccess) IOLog("MT2USBReader: Read arm failed 0x%x\n", r);
}

void com_schmonz_MT2USBReader::readComplete(void *target, void * /*param*/,
                                            IOReturn status, UInt32 remaining) {
    com_schmonz_MT2USBReader *self = (com_schmonz_MT2USBReader *)target;
    if (!self || self->fStopping) return;
    if (status == kIOReturnSuccess) {
        UInt32 cap = self->fMaxPacket ? self->fMaxPacket : 64;
        UInt32 n = cap - remaining;
        if (n > 0) {
            uint8_t buf[256];
            if (n > sizeof(buf)) n = sizeof(buf);
            self->fBuf->readBytes(0, buf, n);
            touch_frame_t tf;
            if (mt2_usb_decode(buf, n, &tf) == 0 && gActiveMT2Gesture)
                gActiveMT2Gesture->submitFrame(self, &tf);   /* self = this reader */
        }
        self->armRead();
    } else if (status != kIOReturnAborted) {
        IOLog("MT2USBReader: read ended 0x%x\n", status);
    }
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

/* Genuine-USB path: manual-start a genuine AppleUSBMultitouchDriver on our interface (bypassing
 * IOKit matching) and interpose its handleReport via an instance-scoped vtable clone, so each MT2
 * report is reframed into the CompactV4 packet Apple accepts. Mirrors MT2BTReader's Path-A manual
 * start + installBnbGeometry safety gate. The genuine driver OWNS the interface + interrupt pipe;
 * our reader must NOT open them in this mode. Install the clone BEFORE start() so the first report
 * is interposed. Best-effort; on any failure we restore-then-release and leave our reader untouched. */
bool com_schmonz_MT2USBReader::startGenuine(IOService *provider) {
    mt2_usb_reframe_reset();          /* fresh per-finger lifecycle history for this stream */
    OSObject *go = OSMetaClass::allocClassWithName("AppleUSBMultitouchDriver");
    IOService *genuine = OSDynamicCast(IOService, go);
    if (!genuine) {
        IOLog("MT2USBReader: allocClassWithName(AppleUSBMultitouchDriver) NULL (AppleUSBMultitouch loaded?)\n");
        if (go) go->release();
        return false;
    }
    /* Seed properties via the INIT dictionary, not setProperty: AppleUSBMultitouchDriver overrides
       every setProperty variant and drops unknown keys (proven: setProperty+getProperty readback
       returned NULL), but init() forwards the dict to super::init which populates the property table
       directly. Manual-start does no personality merge AND the device NAKs Apple's feature reports,
       so the instance otherwise lacks both (a) IOUserClientClass -> IOService::newUserClient has no
       class -> MTDeviceStart's IOServiceOpen fails -> 0 frames clients; and (b) sensor geometry ->
       MultitouchSupport's _mt_CachePropertiesForDevice reads 0x0 cols/rows -> algorithm yields no
       contacts. Supply both here. Geometry values from src/mt2_geometry.c (BT-proven). (RE'd 2026-06-24.) */
    OSDictionary *initp = OSDictionary::withCapacity(24);
    OSString *ucc = OSString::withCString("AppleUSBMultitouchUserClient");
    if (initp && ucc) initp->setObject("IOUserClientClass", ucc);
    if (initp) {
        mt2_dict_num(initp, "Family ID", 0x80);            /* 128 */
        mt2_dict_num(initp, "Sensor Rows", 13);
        mt2_dict_num(initp, "Sensor Columns", 16);
        mt2_dict_num(initp, "Sensor Surface Width", 13000);
        mt2_dict_num(initp, "Sensor Surface Height", 11300);
        mt2_dict_num(initp, "parser-type", 1000);          /* selects the CompactV4 frame parser in */
        mt2_dict_num(initp, "parser-options", 39);         /* 39=0x27, Apple's genuine-USB value; bit 0x2 is the */
                                                           /* "clicky hardware" capability (parser-options -> */
                                                           /* MTSimpleHIDManager::initialize -> this+0xb0, gated by */
                                                           /* handleButtonState/hwSupports*). 37 cleared it -> physical */
                                                           /* + 2-finger click dead while taps worked. (RE'd 2026-06-24.) */
        initp->setObject("Driver is Ready", kOSBooleanTrue);
        initp->setObject("MTHIDDevice", kOSBooleanTrue);

        /* hidd-engagement properties the IOKit personality merge would have supplied — but
         * allocClassWithName manual-start skips that merge. WITHOUT these, hidd never opens an
         * AppleUSBMultitouchUserClient frames client on our instance: no contact frames reach the
         * userspace recognizer (MultitouchHID.plugin) and nothing posts the relative-mouse / scroll
         * events back through the user client -> AppleUSBMultitouchDriver::postRelativeMouseEvent ->
         * the wired event driver -> IOHIDPointing. So no cursor, regardless of the event-driver bind.
         * The working BT AMD carries exactly this set (HIDServiceSupport + IOCFPlugInTypes ->
         * MultitouchHID.plugin) and IS opened by hidd (captures/cursor-seam/A-usb-working-amd.txt).
         * Values from Apple's genuine AppleUSBMultitouchDriver USB personality. (RE'd 2026-06-24.) */
        initp->setObject("HIDServiceSupport", kOSBooleanTrue);
        /* "Trackpad" (NOT the personality's "Mouse"): in "Mouse" behavior the system drives the cursor
         * from the device's own relative-mouse HID reports and uses multitouch only for gestures — but
         * MT2 in multitouch-streaming mode emits NO mouse reports, so the cursor never moves (total
         * silence, observed on-device 2026-06-24). "Trackpad" makes the recognizer synthesize the cursor
         * from the touch frames, exactly as the working BT AMD does (it carries HIDDefaultBehavior=Trackpad). */
        mt2_dict_str(initp, "HIDDefaultBehavior", "Trackpad");
        initp->setObject("TrackpadMomentumScroll", kOSBooleanTrue);
        initp->setObject("TrackpadFourFingerGestures", kOSBooleanTrue);
        initp->setObject("TrackpadSecondaryClickCorners", kOSBooleanTrue);
        initp->setObject("TrackpadThreeFingerDrag", kOSBooleanTrue);
        /* Device-button gate: only TAP gestures worked (pure contact recognition); every PHYSICAL-button
         * action (single click, two-finger right-click) was dead because the device's button state wasn't
         * extracted+posted. This is the BT path's click fix (sets the device-button gate). (2026-06-24.) */
        initp->setObject("ExtractAndPostDeviceButtonState", kOSBooleanTrue);
        /* The recognizer reads gesture/click prefs from MultitouchPreferences (and TrackpadUserPreferences),
         * NOT the top-level flags above; an EMPTY prefs dict can even shadow the live defaults (RE'd on BT).
         * Seed the working set (incl. TrackpadRightClick) under both keys so right-click + clicking enable. */
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
        /* IOCFPlugInTypes tells hidd which CFPlugIn (the multitouch recognizer) to instantiate. */
        OSDictionary *plugins = OSDictionary::withCapacity(1);
        OSString *pluginPath = OSString::withCString(
            "AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin");
        if (plugins && pluginPath) {
            plugins->setObject("0516B563-B15B-11DA-96EB-0014519758EF", pluginPath);
            initp->setObject("IOCFPlugInTypes", plugins);
        }
        if (pluginPath) pluginPath->release();
        if (plugins) plugins->release();
    }
    bool inited = genuine->init(initp);
    if (ucc) ucc->release();
    if (initp) initp->release();
    if (!inited || !genuine->attach(fIntf)) {
        IOLog("MT2USBReader: genuine init/attach failed\n");
        genuine->release();
        return false;
    }
    /* Safety gate: only clone if it really is the class we expect (else a slot-write corrupts
       an unrelated method -> panic). Mirrors installBnbGeometry's class-name check. */
    const char *cls = genuine->getMetaClass()->getClassName();
    if (!cls || strcmp(cls, "AppleUSBMultitouchDriver") != 0) {
        IOLog("MT2USBReader: genuine ABORT — class is '%s'\n", cls ? cls : "(null)");
        genuine->detach(fIntf); genuine->release();
        return false;
    }
    if (vtc_clone_override(genuine, USB_VTABLE_SPAN, USB_HANDLEREPORT_SLOT_INDEX,
                           (void *)&mt2_usb_handle_report, &gUsbVtableClone) != 0) {
        IOLog("MT2USBReader: handleReport clone FAILED to allocate\n");
        genuine->detach(fIntf); genuine->release();
        return false;
    }
    gOrigUsbHandleReport = (usb_handle_report_fn)
        (((void **)gUsbVtableClone.orig_vptr)[USB_HANDLEREPORT_SLOT_INDEX]);
    gUsbVtableCloned = true;

    if (!genuine->start(fIntf)) {
        IOLog("MT2USBReader: genuine start failed\n");
        vtc_restore(genuine, &gUsbVtableClone);     /* restore FIRST, then release */
        gUsbVtableCloned = false; gOrigUsbHandleReport = 0;
        genuine->detach(fIntf); genuine->release();
        return false;
    }
    fGenuine = genuine;

    /* The genuine driver doesn't send MT2's USB multitouch-enable; we do (same as synthetic start). */
    IOUSBDevRequest en;
    uint8_t payload[2] = {0x02, 0x01};
    en.bmRequestType = 0x21; en.bRequest = 0x09;
    en.wValue = 0x0302; en.wIndex = 1; en.wLength = sizeof(payload); en.pData = payload;
    IOReturn enrc = fIntf->DeviceRequest(&en);
    IOLog("MT2USBReader: multitouch-enable SET_REPORT rc=0x%08x\n", enrc);

    /* manual-start via allocClassWithName does NOT merge the driver's IOKit personality, so the
     * instance lacks "IOUserClientClass" — IOService::newUserClient (the driver doesn't override it)
     * then has no class to make and MultitouchSupport's MTDeviceStart->IOServiceOpen fails (0 frames
     * clients => no frames => no cursor/pane). Supply it ourselves so the MT frames user client opens,
     * which kicks registerUserClient->addFramesClient->configureDataMode->streaming. (RE'd 2026-06-24;
     * seeded via the init dict above because the driver's setProperty override drops it.) */

    IOLog("MT2USBReader: genuine AppleUSBMultitouchDriver started + handleReport interposed (slot %lu)\n",
          (unsigned long)USB_HANDLEREPORT_SLOT_INDEX);
    registerService();
    return true;
}

/* Relinquish our exclusive hold on the interface so the provider subtree can
 * finalize. MUST run during the willTerminate handshake (device unplug / re-
 * enumerate): IOKit does not call stop() until our open() is released, so closing
 * only in stop() deadlocks teardown — the reader stays inactive/busy and pins the
 * whole dead device subtree (leaked, never freed; one per unplug). Idempotent.
 * We drop fPipe WITHOUT Abort(): it is unowned and may already be torn down with
 * the device; close() lets the interface abort+close its own pipes. */
void com_schmonz_MT2USBReader::releaseInterface(void) {
    fStopping = true;

    /* Genuine-USB path teardown (reverse startGenuine). Restore the vtable FIRST (vtable_clone.h
     * contract; avoids the in-flight handleReport use-after-free), then terminate + release the
     * genuine driver. In this mode we never opened fIntf/fPipe ourselves, so the synthetic close
     * below is skipped (an unmatched close(this) would unbalance the interface's open count). */
    if (gUsbVtableCloned && fGenuine) {
        vtc_restore(fGenuine, &gUsbVtableClone);
        gUsbVtableCloned = false; gOrigUsbHandleReport = 0;
    }
    if (fGenuine) {
        fGenuine->terminate();
        fGenuine->release();
        fGenuine = 0;
        fIntf = 0;                       /* we never opened it; don't close */
        return;
    }

    fPipe = 0;
    if (fIntf) { fIntf->close(this); fIntf = 0; }
}

bool com_schmonz_MT2USBReader::willTerminate(IOService *provider, IOOptionBits options) {
    releaseInterface();
    return IOService::willTerminate(provider, options);
}

void com_schmonz_MT2USBReader::stop(IOService *provider) {
    releaseInterface();                          /* no-op if willTerminate already did it */
    if (fBuf) { fBuf->release(); fBuf = 0; }     /* safe now: interface closed, no I/O in flight */
    IOLog("MT2USBReader: stopped\n");
    IOService::stop(provider);
}
