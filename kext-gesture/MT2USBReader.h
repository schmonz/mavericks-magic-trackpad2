#ifndef MT2USBREADER_H
#define MT2USBREADER_H
#include <IOKit/IOService.h>
/* The Kernel.framework USB headers derive from these IOKit base classes but don't
   pull them in themselves; include them first or IOUSBInterface.h fails to compile
   (IOCommand/IOCommandPool "unknown class name"). */
#include <IOKit/IOCommand.h>
#include <IOKit/IOCommandPool.h>
#include <IOKit/usb/IOUSBInterface.h>
#include <IOKit/usb/IOUSBPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "genuine_host.h"          /* shared manual-start + ordered-teardown core */

/* Active in-kernel USB transport. Matches interface 1 (idVendor 1452 / idProduct
   613) at high probe score, opens the interrupt-IN pipe, sends the SET_REPORT
   multitouch-enable, async-reads frames (armRead/readComplete -> mt2_usb_decode ->
   submitFrame), and drives a SP1-hardened fabricated AppleMultitouchDevice (gUsbAmdCtx)
   via kUsbSink. No genuine AppleUSBMultitouchDriver is started. Genuine machinery kept
   compiling; deleted in Task 2. */
class com_schmonz_MT2USBReader : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2USBReader)
    IOUSBInterface *fIntf;
    IOUSBPipe      *fPipe;
    IOBufferMemoryDescriptor *fBuf;
    UInt32          fMaxPacket;
    bool            fStopping;
    IOService *fGenuine;           /* genuine AppleUSBMultitouchDriver — Task 2 deletes */
    gh_host_t fHost;               /* genuine_host lifecycle handle for fGenuine */
    void armRead(void);
    static void readComplete(void *target, void *param, IOReturn status, UInt32 remaining);
    void releaseInterface(void);   /* reverse startGenuine; idempotent (Task 2 deletes) */
    bool startGenuine(IOService *provider);   /* manual-start + interpose (Task 2 deletes) */
    /* startGenuine as named steps (named alike to MT2BTReader where the action matches): */
    void resetTransportState(void);       /* fresh per-stream reframe + button-edge state */
    void sendEnable(void);                /* MT2 USB multitouch-enable (control transfer) */
    void settle(void);                    /* let the device leave mouse mode before the AMD probes */
    bool manualStartGenuineAmd(void);     /* host a genuine AppleUSBMultitouchDriver on the interface */
public:
    virtual bool start(IOService *provider) override;
    /* Release the interface here, NOT just in stop(): on device unplug/re-enumerate
       IOKit defers stop() until the interface is relinquished, so a stop()-only
       teardown deadlocks and leaks the dead device subtree. */
    virtual bool willTerminate(IOService *provider, IOOptionBits options) override;
    virtual void stop(IOService *provider) override;
};
#endif
