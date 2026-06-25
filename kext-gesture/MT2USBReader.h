#ifndef MT2USBREADER_H
#define MT2USBREADER_H
#include <IOKit/IOService.h>
/* The Kernel.framework USB headers derive from these IOKit base classes but don't
   pull them in themselves; include them first or IOUSBInterface.h fails to compile
   (IOCommand/IOCommandPool "unknown class name"). */
#include <IOKit/IOCommand.h>
#include <IOKit/IOCommandPool.h>
#include <IOKit/usb/IOUSBInterface.h>
#include "genuine_host.h"          /* shared manual-start + ordered-teardown core */

/* Active in-kernel USB transport. Matches interface 1 (idVendor 1452 / idProduct
   613) at high probe score, then manual-starts Apple's genuine AppleUSBMultitouchDriver
   on the interface and interposes its handleReport (the CompactV4 reframe seam) so each
   MT2 0x02 report is reframed into the packet Apple's recognizer accepts. The manual-start
   + interpose + ordered teardown run through the shared genuine_host core (fHost + the
   gh_* adapter callbacks). */
class com_schmonz_MT2USBReader : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2USBReader)
    IOUSBInterface *fIntf;
    IOService *fGenuine;           /* genuine AppleUSBMultitouchDriver, manual-started */
    gh_host_t fHost;               /* genuine_host lifecycle handle for fGenuine */
    void releaseInterface(void);   /* reverse startGenuine; idempotent */
    bool startGenuine(IOService *provider);   /* manual-start + interpose Apple's driver */

    /* genuine_host adapter callbacks (ctx = this reader, obj = the genuine driver). */
    static void *gh_alloc(void *ctx, const char *cls);
    static bool  gh_class_ok(void *ctx, void *obj, const char *e);
    static bool  gh_init_attach(void *ctx, void *obj);
    static int   gh_interpose(void *ctx, void *obj);
    static bool  gh_start_drv(void *ctx, void *obj);
    static void  gh_restore(void *ctx, void *obj);
    static void  gh_detach(void *ctx, void *obj);
    static void  gh_terminate(void *ctx, void *obj);
    static void  gh_release(void *ctx, void *obj);
    static const gh_adapter_t kUsbAdapter;   /* the nine callbacks above, as a gh_adapter_t */
public:
    virtual bool start(IOService *provider) override;
    /* Release the interface here, NOT just in stop(): on device unplug/re-enumerate
       IOKit defers stop() until the interface is relinquished, so a stop()-only
       teardown deadlocks and leaks the dead device subtree. */
    virtual bool willTerminate(IOService *provider, IOOptionBits options) override;
    virtual void stop(IOService *provider) override;
};
#endif
