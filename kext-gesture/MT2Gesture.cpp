/*
 * MT2Gesture - research kext (milestone 2: start + register user client).
 *
 * Goal: play the role Apple's BluetoothMultitouchTransport plays - construct and
 * drive an AppleMultitouchDevice ourselves so the real gesture engine
 * (MultitouchSupport) engages, instead of faking a USB/BT transport.
 *
 * RE finding (this session) on AppleMultitouchDevice::start(IOService*):
 *   - it reads getProperty("IsFake").
 *   - IsFake==false -> STRICT path: walks the IOService plane to the parent
 *     provider and requires it cast to AppleMultitouchHIDEventDriverV2 /
 *     AppleMultitouchHIDEventDriver / AppleMultitouchHIDEventService, else
 *     "Could not cast our provider" -> "Failing start." -> returns false.
 *   - IsFake==true -> LENIENT path: best-effort event-service lookup but ALWAYS
 *     falls through to continue start() successfully regardless of provider.
 *   - IOUserClientClass = AppleMultitouchDeviceUserClient is set BEFORE that
 *     check, so the user client registers either way.
 * We are a plain IOService, so we set IsFake=true to bypass the cast and still
 * get the device to start and register its AppleMultitouchDeviceUserClient ->
 * MTDeviceCreateList can see it. Fake mode skips the in-kernel gesture-event
 * wrapper (gesture -> IOHIDSystem); the userspace MultitouchSupport path likely
 * does not need it - revisit at M4.
 *
 * Milestone 1b proved alloc+init is panic-free (init must precede any free()).
 * This file adds: IsFake dict, handler stubs, attach, start, registerService,
 * and a real stop() teardown (device is init'd+started so free() is safe).
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <kern/clock.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "amd_shim.h"
#include "MT2Gesture.h"
#include "MT2HIDShell.h"
#include "mt1_encode.h"

OSDefineMetaClassAndStructors(com_schmonz_MT2Gesture, IOService)

/* Kernel uptime in milliseconds — the clock the session reads through the shell. */
static uint32_t uptime_ms(void) {
    uint64_t abs_t, ns;
    clock_get_uptime(&abs_t);
    absolutetime_to_nanoseconds(abs_t, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

/* Sink: post a device-button edge (mask 0/0x1/0x2) through the native path. */
void com_schmonz_MT2Gesture::sink_post_click(void *ctx, unsigned mask) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (self->fDevice) self->fDevice->handlePointerEventFromDevice(0, 0, mask, 0);
}
/* Sink: MT1-encode the touch frame and feed it to the device verbatim. */
void com_schmonz_MT2Gesture::sink_feed_frame(void *ctx, const touch_frame_t *frame) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (!self->fDevice) return;
    uint8_t mt1[256];
    int n = mt1_encode(frame, mt1, sizeof(mt1), uptime_ms());
    if (n > 0) self->fDevice->handleTouchFrame(mt1, (unsigned int)n);
}
/* Sink: (re)arm the silence-watchdog timer. */
void com_schmonz_MT2Gesture::sink_arm_timer(void *ctx, uint32_t ms) {
    com_schmonz_MT2Gesture *self = (com_schmonz_MT2Gesture *)ctx;
    if (self->fIdleTimer) self->fIdleTimer->setTimeoutMS(ms);
}

/* A reader (BT/USB) announces its transport; the session resets and arms settle. */
void com_schmonz_MT2Gesture::connectionEstablished(IOService *source,
                                                   mt2_transport_mode_t mode) {
    if (fSessionLock) IOLockLock(fSessionLock);
    if (fIdleTimer) fIdleTimer->cancelTimeout();
    mt2_session_connect(&fSession, (uintptr_t)source, mode, uptime_ms());
    if (fSessionLock) IOLockUnlock(fSessionLock);
    IOLog("MT2Gesture: connection established (src=%p mode=%d)\n", source, (int)mode);
}
/* A reader submits one decoded frame; the session decides what reaches the device. */
void com_schmonz_MT2Gesture::submitFrame(IOService *source, const touch_frame_t *tf) {
    if (fSessionLock) IOLockLock(fSessionLock);
    mt2_session_frame(&fSession, (uintptr_t)source, tf, uptime_ms(), &fSink);
    if (fSessionLock) IOLockUnlock(fSessionLock);
}
/* The silence-watchdog timer fired; let the session flush any outstanding BreakTouch. */
void com_schmonz_MT2Gesture::idleTimeout(OSObject *owner, IOTimerEventSource * /*s*/) {
    com_schmonz_MT2Gesture *self = OSDynamicCast(com_schmonz_MT2Gesture, owner);
    if (!self) return;
    if (self->fSessionLock) IOLockLock(self->fSessionLock);
    mt2_session_timer(&self->fSession, &self->fSink);
    if (self->fSessionLock) IOLockUnlock(self->fSessionLock);
}

/* The active gesture nub, published for the in-kernel readers (MT2BTReader and
 * MT2USBReader) to feed via submitFrame() — same kext, so no user client / IPC.
 * Single instance. */
com_schmonz_MT2Gesture *gActiveMT2Gesture = 0;

/* Build the property table an IOHIDDevice needs to look like the BT Magic Trackpad
 * (matches BNBTrackpadEventDriver: VendorID 1452, VendorIDSource 2, usage 1/2,
 * Transport Bluetooth). Mirrors src/vhid_mt1.c's creation dict. */
static OSDictionary *makeHidProps(void) {
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
    OSString *t = OSString::withCString("Bluetooth");
    if (t) { p->setObject("Transport", t); t->release(); }
    OSString *pr = OSString::withCString("Magic Trackpad");
    if (pr) { p->setObject("Product", pr); pr->release(); }
    OSString *mf = OSString::withCString("Apple Inc.");
    if (mf) { p->setObject("Manufacturer", mf); mf->release(); }
    return p;
}

/* AMDDeviceReportStruct layout (from Apple staticGet/SetReportHandler):
 *   off 0x000  uint8  reportID
 *   off 0x001  uint8  buf[0x203]   (report data)
 *   off 0x204  uint32 length
 * INSTRUMENTED PASS: log every report the driver asks for so we learn which IDs
 * cacheDeviceProperties fetches. get-report returns an empty (len=0) report. */
static int enableStub(bool enable, void *t) {
    (void)t;
    IOLog("MT2Gesture: ENABLE-MT enable=%d\n", (int)enable);
    return 0;
}
/* Remember the 1-byte value hidd SETs per reportID, so a later GET reads it back
 * (a real device echoes its mode registers; hidd SETs 0xC8/0xDC/0xDD then GETs them
 * and may disable gestures if the GET fails). */
static unsigned char g_reg[256];
/* Geometry get-report handler. Report->key mapping reversed from
 * AppleMultitouchDevice::decodeDeviceProperty (D-report jump table @0x3bc4):
 *   0xD1 -> Family ID (data[0])
 *   0xD3 -> Endianness(d0) Rows(d1) Columns(d2) bcdVersion(d3..d4 BE)  [PacketSize defaults 0x400]
 *   0xD9 -> Surface Width(u32 d0..3) Height(u32 d4..7) Descriptor(whole if len>=16)
 *   0xD0 -> Sensor Region Descriptor (raw)   0xA1 -> Sensor Region Param (raw)
 *   0x7F -> rCRITICAL_ERRORS (u32 must be 0)  0xDB -> Multitouch ID (skip => return error)
 * Data is written at r[1]; the response length goes in *(u32*)(r+0x204). First
 * attempt at values (Magic-Trackpad-ish); tune empirically vs MTDeviceCreateList. */
static int getReportStub(AMDDeviceReportStruct *r, unsigned char id, void *t) {
    (void)t; (void)id;
    unsigned char *b = (unsigned char *)r;
    unsigned char rid = b[0];
    unsigned char *o = b + 1;                      /* response data buffer */
    unsigned int *lenp = (unsigned int *)(b + 0x204);
    unsigned int n = 0, i;
    switch (rid) {
    case 0x7f:                                     /* rCRITICAL_ERRORS: none */
        o[0]=o[1]=o[2]=o[3]=0; n=4; break;
    case 0xd1:                                     /* Family ID */
        o[0]=0x80; n=1; break;
    case 0xd3:                                     /* Endianness,Rows,Cols,bcdVersion */
        o[0]=0x01; o[1]=0x0d; o[2]=0x10; o[3]=0x01; o[4]=0x00; n=5; break;
    case 0xd9: {                                   /* Surface Width/Height (u32 LE) */
        unsigned int w=13000, h=11300;
        o[0]=w&0xff; o[1]=(w>>8)&0xff; o[2]=(w>>16)&0xff; o[3]=(w>>24)&0xff;
        o[4]=h&0xff; o[5]=(h>>8)&0xff; o[6]=(h>>16)&0xff; o[7]=(h>>24)&0xff;
        n=8; break; }
    case 0xd0:                                     /* Sensor Region Descriptor */
    case 0xa1:                                     /* Sensor Region Param */
        for (i=0;i<16;i++) o[i]=0; n=16; break;
    case 0xdb:                                     /* Multitouch ID: let driver skip */
        IOLog("MT2Gesture: GET-REPORT id=0xdb UNHANDLED -> skip\n");
        return (int)0xe00002c7;                    /* kIOReturnUnsupported */
    default:                                       /* echo back the last SET value */
        o[0] = g_reg[rid]; n = 1;
        IOLog("MT2Gesture: GET-REPORT id=0x%02x -> echo 0x%02x\n", (unsigned)rid, (unsigned)o[0]);
        break;
    }
    *lenp = n;
    IOLog("MT2Gesture: GET-REPORT id=0x%02x -> %u bytes\n", (unsigned)rid, n);
    return 0;
}
static int setReportStub(AMDDeviceReportStruct *r, unsigned char id, void *t) {
    (void)t;
    unsigned char *b = (unsigned char *)r;
    g_reg[b[0]] = b[1];                             /* remember it for the echo on GET */
    IOLog("MT2Gesture: SET-REPORT typeArg=%u reportID=0x%02x len=%u b1=0x%02x\n",
          (unsigned)id, (unsigned)b[0], *(unsigned int *)(b + 0x204), (unsigned)b[1]);
    return 0;
}

bool com_schmonz_MT2Gesture::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    fDevice = 0;
    fHidShell = 0;
    gActiveMT2Gesture = this;   /* let the in-kernel readers feed us */

    /* Functional-core init + the sink that drives IOKit, plus the silence-watchdog
     * timer the session arms. The session owns all post-decode logic; this shell only
     * supplies the clock, the source token, and these effect callbacks. */
    fSession.active_source = 0;
    fSession.mode = MT2_EVENT_DRIVEN;
    fSession.settle_until_ms = 0;
    fSession.last_button = 0;
    mt2_lifecycle_reset(&fSession.lifecycle);
    fSink.post_click = &com_schmonz_MT2Gesture::sink_post_click;
    fSink.feed_frame = &com_schmonz_MT2Gesture::sink_feed_frame;
    fSink.arm_timer  = &com_schmonz_MT2Gesture::sink_arm_timer;
    fSink.ctx = this;
    fSessionLock = IOLockAlloc();   /* serializes timer vs submitFrame fSession access */
    fPipeWL = IOWorkLoop::workLoop();
    fIdleTimer = 0;
    if (fPipeWL) {
        fIdleTimer = IOTimerEventSource::timerEventSource(
            this, &com_schmonz_MT2Gesture::idleTimeout);
        if (fIdleTimer && fPipeWL->addEventSource(fIdleTimer) != kIOReturnSuccess) {
            fIdleTimer->release(); fIdleTimer = 0;
        }
    }

    /* Publish ourselves so the in-kernel readers' providers resolve and IOKit
     * finishes matching our subtree. */
    registerService();

    /* M5: stand up an in-kernel MT1 HID device UNDER US so Apple's
     * AppleMultitouchHIDEventDriver matches+starts a real IOHIDEventService
     * (IOHIDPointing) in our subtree. That started driver is what
     * AppleMultitouchDevice's (non-fake) wrapper wiring attaches to for actuation.
     * Best-effort: if any of this fails we still register the device (M4 path). */
    {
        com_schmonz_MT2HIDShell *hid = new com_schmonz_MT2HIDShell;
        OSDictionary *hp = makeHidProps();
        if (hid && hp && hid->init(hp)) {
            if (hid->attach(this) && hid->start(this)) {
                fHidShell = hid;
                IOLog("MT2Gesture: MT1 HID shell started under nub\n");
            } else {
                IOLog("MT2Gesture: MT1 HID shell attach/start FAILED\n");
                hid->detach(this);
                hid->release();
                hid = 0;
            }
        } else {
            IOLog("MT2Gesture: MT1 HID shell init/alloc FAILED\n");
            if (hid) hid->release();
        }
        if (hp) hp->release();
    }

    OSObject *o = OSMetaClass::allocClassWithName("AppleMultitouchDevice");
    if (!o) {
        IOLog("MT2Gesture: allocClassWithName(AppleMultitouchDevice) NULL\n");
        return true;
    }
    IOService *dev = OSDynamicCast(IOService, o);
    if (!dev) {
        IOLog("MT2Gesture: alloc is not an IOService\n");
        o->release();
        return true;
    }
    /* Layout-compatible reinterpret: AppleMultitouchDevice single-inherits
     * IOService at offset 0, so the pointer is identical. The shim adds only
     * non-virtual method declarations (resolved to exported symbols); it does
     * not alter the real object's vtable. */
    AppleMultitouchDevice *amd = (AppleMultitouchDevice *)dev;

    OSDictionary *props = OSDictionary::withCapacity(2);
    if (!props) {
        IOLog("MT2Gesture: dict alloc failed\n");
        dev->release();
        return true;
    }
    props->setObject("IsFake", kOSBooleanFalse);   /* bypass start() provider cast */

    bool ok = dev->init(props);                    /* virtual -> AppleMultitouchDevice::init */
    props->release();
    IOLog("MT2Gesture: AppleMultitouchDevice init -> %d (dev=%p)\n", ok, dev);
    if (!ok) {
        dev->release();
        return true;
    }

    /* Belt-and-suspenders: ensure IsFake is in the device property table even if
     * init() did not adopt our dict wholesale. start() reads it via getProperty. */
    dev->setProperty("IsFake", kOSBooleanFalse);

    /* Physical-click reliability: start() reads getProperty("ExtractAndPostDeviceButtonState")
     * (== kOSBooleanTrue) and, when true, sets a flag (this+0xb0 struct, byte +9) that makes
     * handlePointerEventFromDevice DISPATCH the device button immediately on the press/release
     * edge -- not only when a later motion frame happens to OR it into the output. Without it,
     * quick stationary taps (no motion between press and release) are dropped (RE'd live). Must
     * be present before start() runs its gate check, hence set here alongside IsFake. */
    dev->setProperty("ExtractAndPostDeviceButtonState", kOSBooleanTrue);

    amd->setEnableMultitouchHandler(&enableStub, this);
    amd->setGetReportHandler(&getReportStub, this);
    amd->setSetReportHandler(&setReportStub, this);

    if (!dev->attach(this)) {
        IOLog("MT2Gesture: device attach failed\n");
        dev->release();
        return true;
    }
    if (!dev->start(this)) {
        IOLog("MT2Gesture: device start FAILED (cast/provider?)\n");
        dev->detach(this);
        dev->release();
        return true;
    }
    /* M5 adoption probe: MultitouchSupport's MTDeviceIsBuiltIn reads the device
     * registry property "MT Built-In"; the system auto-drives built-in trackpads
     * regardless of the saved "default device" pref. "Driver is Ready" is also
     * cached by mt_CachePropertiesForDevice. Set both and see if WindowServer
     * opens our AppleMultitouchDeviceUserClient (= adopts the device). */
    dev->setProperty("MT Built-In", kOSBooleanTrue);
    dev->setProperty("Driver is Ready", kOSBooleanTrue);

    /* M5 ADOPTION FIX (diffed vs a real hidd-adopted BNBTrackpadDevice device):
     * the adopted device advertises IOCFPlugInTypes -> MultitouchHID.plugin, which is
     * how MultitouchSupport/hidd instantiates the plugin to open a multitouch device.
     * Without it hidd never adopts us. Mirror the stock DefaultMultitouchProperties. */
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
    dev->setProperty("TrackpadFourFingerGestures", kOSBooleanTrue);
    dev->setProperty("TrackpadMomentumScroll", kOSBooleanTrue);
    /* Rest of the stock DefaultMultitouchProperties (the BNBTrackpadDriver personality):
     * parser-type/options tell MultitouchSupport how to parse the device; MTHIDDevice /
     * HIDServiceSupport flag it as a real MT HID trackpad. */
    {
        OSNumber *pt = OSNumber::withNumber((unsigned long long)1000, 32);
        if (pt) { dev->setProperty("parser-type", pt); pt->release(); }
        OSNumber *po = OSNumber::withNumber((unsigned long long)47, 32);
        if (po) { dev->setProperty("parser-options", po); po->release(); }
    }
    dev->setProperty("MTHIDDevice", kOSBooleanTrue);
    dev->setProperty("HIDServiceSupport", kOSBooleanTrue);
    dev->setProperty("TrackpadSecondaryClickCorners", kOSBooleanTrue);
    /* Tell the system to treat this as a TRACKPAD (cursor/gestures), not a generic
     * digitizer. hidd forwards our touches as raw DigitizerEvents but runs no
     * gesture/cursor recognition without this (stock BNBTrackpadDriver sets it). */
    dev->setProperty("HIDDefaultBehavior", "Trackpad");

    /* GESTURE/CURSOR ACTIVATION (RE'd from MTTrackpadHIDManager::determineHIDManagerSettings
     * in MultitouchHID.plugin): the userspace gesture recognizer (hidd) builds its
     * trackpad settings + chord-gesture-set by reading a "TrackpadUserPreferences"
     * dictionary from THIS device's IORegistry entry via
     * IORegistryEntryCreateCFProperty(MTDeviceGetService(d), "TrackpadUserPreferences").
     * With neither "TrackpadUserPreferences" nor "MultitouchPreferences" present it runs a
     * bare-defaults path that leaves the chord set empty -> no chord ever commits ->
     * no cursor/tap/scroll/gesture output (confirmed via dtrace: handleEvent + hand stats
     * run, but commit2Chord/dispatchEvents/AppendRelativeMouse never fire). Provide the
     * dict with sensible enabled values so the recognizer populates a real gesture set. */
    {
        OSDictionary *tp = OSDictionary::withCapacity(24);
        if (tp) {
            #define MT_SET_BOOL(k, v) do { tp->setObject(k, (v) ? kOSBooleanTrue : kOSBooleanFalse); } while (0)
            #define MT_SET_INT(k, v) do { OSNumber *n = OSNumber::withNumber((unsigned long long)(v), 32); \
                if (n) { tp->setObject(k, n); n->release(); } } while (0)
            MT_SET_BOOL("Clicking", true);            /* tap to click */
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
            dev->setProperty("TrackpadUserPreferences", tp);
            tp->release();
            IOLog("MT2Gesture: TrackpadUserPreferences installed (gesture activation)\n");
        }
    }

    dev->registerService();
    fDevice = amd;
    IOLog("MT2Gesture: AppleMultitouchDevice started + registered (fake mode)\n");
    return true;
}

void com_schmonz_MT2Gesture::stop(IOService *provider) {
    /* Tear down the HID shell first: terminating it propagates down to Apple's
     * AppleMultitouchHIDEventDriver, whose termination fires the device's
     * hidEventDriverTerminated notification and cleanly clears the wrapper before
     * we release the AppleMultitouchDevice below. */
    if (fHidShell) {
        fHidShell->terminate();
        fHidShell->release();
        fHidShell = 0;
        IOLog("MT2Gesture: MT1 HID shell terminated + released\n");
    }
    /* Tear the watchdog timer down BEFORE releasing fDevice so a late fire can't drive a
     * freed device through the sink. */
    if (fIdleTimer) {
        fIdleTimer->cancelTimeout();
        if (fPipeWL) fPipeWL->removeEventSource(fIdleTimer);
        fIdleTimer->release(); fIdleTimer = 0;
    }
    if (fPipeWL) { fPipeWL->release(); fPipeWL = 0; }
    /* Timer is fully removed above (no more idleTimeout), so the lock has no more
     * users and is safe to free. */
    if (fSessionLock) { IOLockFree(fSessionLock); fSessionLock = 0; }
    if (fDevice) {
        IOService *dev = (IOService *)fDevice;
        dev->stop(this);
        dev->detach(this);
        dev->release();
        fDevice = 0;
        IOLog("MT2Gesture: device stopped + released\n");
    }
    if (gActiveMT2Gesture == this) gActiveMT2Gesture = 0;
    IOService::stop(provider);
}

extern "C" {
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.schmonz.MT2Gesture, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
