/*
 * MT2StartMT - experiment helper (Approach B). When a BNBTrackpadDevice instance
 * appears (our injected personality makes Apple's BT trackpad transport match the
 * Magic Trackpad 2's L2CAP channel), call BluetoothMultitouchTransport::startMultitouch()
 * on it DIRECTLY - bypassing the BT handshake that the MT2 fails. startMultitouch ->
 * createMultitouchHandler builds an AppleMultitouchDevice from the STATIC personality
 * dict (multitouchProperties() reads DefaultMultitouchProperties/Preferences, no live
 * GET_REPORTs), parented under the real BT transport. Goal: see whether THAT
 * transport-constructed device gets adopted by the gesture consumer (the thing nothing
 * ever does for our synthetic M5 device), to find the adoption secret.
 *
 * startMultitouch is BNBTrackpadDevice vtable offset 0xd10 (byte-verified on this box:
 * AppleBluetoothMultitouch vtable for BNBTrackpadDevice @0xa280, obj vptr 0xa290,
 * slot 0xaf90=multitouchProperties/0xd00, 0xaf98=createMultitouchHandler/0xd08,
 * 0xafa0=startMultitouch/0xd10). Version-fragile; transient /tmp load only.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSDictionary.h>

#define BNB_STARTMULTITOUCH_VTABLE_OFFSET 0xd10

class com_schmonz_MT2StartMT : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2StartMT)
    IONotifier *fNotif;
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    static bool onBNB(void *target, void *ref, IOService *svc, IONotifier *n);
};

OSDefineMetaClassAndStructors(com_schmonz_MT2StartMT, IOService)

bool com_schmonz_MT2StartMT::onBNB(void *target, void *ref, IOService *svc, IONotifier *n) {
    (void)target; (void)ref; (void)n;
    if (!svc) return true;
    /* Resolve startMultitouch through the instance's own vtable (auto-relocated to the
     * loaded AppleBluetoothMultitouch address) and call it: void startMultitouch(this). */
    char *obj = (char *)svc;
    void **vtable = *(void ***)obj;
    typedef void (*startfn_t)(void *);
    startfn_t startMT = (startfn_t)vtable[BNB_STARTMULTITOUCH_VTABLE_OFFSET / sizeof(void *)];
    IOLog("MT2StartMT: BNBTrackpadDevice %p published; vtable=%p slot0xd10=%p -> calling startMultitouch\n",
          svc, (void *)vtable, (void *)startMT);
    startMT(svc);
    IOLog("MT2StartMT: startMultitouch() returned for %p\n", svc);
    return true;
}

bool com_schmonz_MT2StartMT::start(IOService *provider) {
    if (!IOService::start(provider)) {
        return false;
    }
    fNotif = 0;
    OSDictionary *m = serviceMatching("BNBTrackpadDevice");
    if (m) {
        fNotif = addMatchingNotification(gIOFirstPublishNotification, m,
                                         &com_schmonz_MT2StartMT::onBNB, this, 0);
        m->release();
    }
    IOLog("MT2StartMT: armed BNBTrackpadDevice publish watch (notifier=%p)\n", fNotif);
    registerService();
    return true;
}

void com_schmonz_MT2StartMT::stop(IOService *provider) {
    if (fNotif) {
        fNotif->remove();
        fNotif = 0;
    }
    IOLog("MT2StartMT: stopped\n");
    IOService::stop(provider);
}

extern "C" {
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(com.schmonz.MT2StartMT, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
}
