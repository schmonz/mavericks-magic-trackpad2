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
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSBoolean.h>
#include "amd_shim.h"

class com_schmonz_MT2Gesture : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2Gesture)
    AppleMultitouchDevice *fDevice;
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(com_schmonz_MT2Gesture, IOService)

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
    default:
        IOLog("MT2Gesture: GET-REPORT id=0x%02x UNHANDLED -> skip\n", (unsigned)rid);
        return (int)0xe00002c7;                    /* kIOReturnUnsupported */
    }
    *lenp = n;
    IOLog("MT2Gesture: GET-REPORT id=0x%02x -> %u bytes\n", (unsigned)rid, n);
    return 0;
}
static int setReportStub(AMDDeviceReportStruct *r, unsigned char id, void *t) {
    (void)t;
    unsigned char *b = (unsigned char *)r;
    IOLog("MT2Gesture: SET-REPORT typeArg=%u reportID=0x%02x len=%u b1=0x%02x\n",
          (unsigned)id, (unsigned)b[0], *(unsigned int *)(b + 0x204), (unsigned)b[1]);
    return 0;
}

bool com_schmonz_MT2Gesture::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    fDevice = 0;

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
    props->setObject("IsFake", kOSBooleanTrue);   /* bypass start() provider cast */

    bool ok = dev->init(props);                    /* virtual -> AppleMultitouchDevice::init */
    props->release();
    IOLog("MT2Gesture: AppleMultitouchDevice init -> %d (dev=%p)\n", ok, dev);
    if (!ok) {
        dev->release();
        return true;
    }

    /* Belt-and-suspenders: ensure IsFake is in the device property table even if
     * init() did not adopt our dict wholesale. start() reads it via getProperty. */
    dev->setProperty("IsFake", kOSBooleanTrue);

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
    dev->registerService();
    fDevice = amd;
    IOLog("MT2Gesture: AppleMultitouchDevice started + registered (fake mode)\n");
    return true;
}

void com_schmonz_MT2Gesture::stop(IOService *provider) {
    if (fDevice) {
        IOService *dev = (IOService *)fDevice;
        dev->stop(this);
        dev->detach(this);
        dev->release();
        fDevice = 0;
        IOLog("MT2Gesture: device stopped + released\n");
    }
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
