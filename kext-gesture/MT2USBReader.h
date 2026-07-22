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

/* Active in-kernel USB transport. Matches interface 1 (idVendor 1452 / idProduct
   613) at high probe score, opens the interrupt-IN pipe, sends the SET_REPORT
   multitouch-enable, async-reads frames (armRead/readComplete -> mt2_usb_decode ->
   submitFrame), and drives a SP1-hardened fabricated AppleMultitouchDevice (gUsbAmdCtx)
   via kUsbSink. No genuine AppleUSBMultitouchDriver is started. */
class MT2USBReader : public IOService {
    OSDeclareDefaultStructors(MT2USBReader)
    IOUSBInterface *fIntf;
    IOUSBPipe      *fPipe;
    IOBufferMemoryDescriptor *fBuf;
    UInt32          fMaxPacket;
    bool            fStopping;
    IOService      *fMux;          /* bound VoodooInput mux (found lazily by VOODOO_INPUT_IDENTIFIER) */
    void armRead(void);
    static void readComplete(void *target, void *param, IOReturn status, UInt32 remaining);
    void releaseInterface(void);   /* abort pipe + close interface; idempotent */
public:
    virtual bool start(IOService *provider) override;
    /* Release the interface here, NOT just in stop(): on device unplug/re-enumerate
       IOKit defers stop() until the interface is relinquished, so a stop()-only
       teardown deadlocks and leaks the dead device subtree. */
    virtual bool willTerminate(IOService *provider, IOOptionBits options) override;
    virtual void stop(IOService *provider) override;
};
#endif
