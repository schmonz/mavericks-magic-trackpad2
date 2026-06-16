/*
 * MT2USBClaim — a deliberately inert kernel driver.
 *
 * Its only job is to win the IOKit match for the Magic Trackpad 2's HID USB
 * interfaces (via a higher IOProbeScore than IOUSBHIDDriver) and then do
 * NOTHING — in particular, it never opens the interface. That keeps the
 * generic kernel HID driver off the device and leaves the USB pipes free for
 * our userspace daemon to claim and read raw multitouch frames.
 *
 * No I/O, no callbacks, no allocations beyond construction => negligible panic
 * risk. All real logic lives in userspace.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

class com_schmonz_MT2USBClaim : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2USBClaim)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(com_schmonz_MT2USBClaim, IOService)

bool com_schmonz_MT2USBClaim::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    IOLog("MT2USBClaim: claimed %s; not opening, left free for userspace\n",
          provider ? provider->getName() : "(null)");
    return true;
}

void com_schmonz_MT2USBClaim::stop(IOService *provider) {
    IOLog("MT2USBClaim: releasing %s\n", provider ? provider->getName() : "(null)");
    IOService::stop(provider);
}

/* kmod boilerplate for a manually-built kext. libkmodc++ supplies _start/_stop
 * (they run the C++ static constructors that register the OSMetaClass). */
extern "C" {
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.schmonz.MT2USBClaim, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
