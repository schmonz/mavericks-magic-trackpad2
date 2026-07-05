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
    /* startGenuine as named steps (named alike to MT2BTReader where the action matches): */
    void resetTransportState(void);       /* fresh per-stream reframe + button-edge state */
    void sendEnable(void);                /* MT2 USB multitouch-enable (control transfer) */
    void settle(void);                    /* let the device leave mouse mode before the AMD probes */
    bool manualStartGenuineAmd(void);     /* host a genuine AppleUSBMultitouchDriver on the interface */
    void armAbsencePump(void);            /* post-liftoff absence-frame heartbeat */
public:
    virtual bool start(IOService *provider) override;
    /* Release the interface here, NOT just in stop(): on device unplug/re-enumerate
       IOKit defers stop() until the interface is relinquished, so a stop()-only
       teardown deadlocks and leaks the dead device subtree. */
    virtual bool willTerminate(IOService *provider, IOOptionBits options) override;
    virtual void stop(IOService *provider) override;
};
#endif
