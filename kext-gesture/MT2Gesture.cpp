/*
 * MT2Gesture — research kext (milestone 1).
 *
 * Goal of the whole effort: play the role Apple's BluetoothMultitouchTransport
 * plays — construct and drive an AppleMultitouchDevice ourselves so the real
 * gesture engine (MultitouchSupport) engages, instead of faking a USB/BT
 * transport.
 *
 * Milestone 1b (this file): milestone 1 (bare alloc+release) PANICKED -
 * AppleMultitouchDevice::free() walks members that init() IOMallocs, so a
 * non-init'd object NULL-derefs. So here: alloc -> init(dict) and DELIBERATELY
 * do NOT release (leak it) to isolate "does init() panic?" from "does free()
 * panic?". If this loads cleanly and logs init->1, init is safe and we move on
 * to attach/start. Match IOResources so start() runs once at load.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>

class com_schmonz_MT2Gesture : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2Gesture)
public:
    virtual bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(com_schmonz_MT2Gesture, IOService)

bool com_schmonz_MT2Gesture::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    OSObject *o = OSMetaClass::allocClassWithName("AppleMultitouchDevice");
    if (!o) {
        IOLog("MT2Gesture: allocClassWithName(AppleMultitouchDevice) NULL\n");
        return true;
    }
    IOService *dev = OSDynamicCast(IOService, o);
    if (!dev) {
        IOLog("MT2Gesture: alloc is not an IOService (leaking, cannot free safely)\n");
        return true;
    }
    OSDictionary *props = OSDictionary::withCapacity(4);
    bool ok = dev->init(props);          /* virtual -> AppleMultitouchDevice::init */
    IOLog("MT2Gesture: AppleMultitouchDevice init -> %d (dev=%p)\n", ok, dev);
    if (props) props->release();
    /* Intentionally NOT releasing dev: until we trust init() fully set up the
     * members free() walks, calling free() risks the milestone-1 panic. Leak it
     * for this isolation test; teardown comes once init/attach/start are sound. */
    return true;
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
