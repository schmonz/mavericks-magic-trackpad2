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
   613) at high probe score, opens the interrupt-IN pipe, sends the enable-MT
   control request, async-reads frames; each is decoded (mt2_usb_decode) and pushed
   to the MT2Gesture nub as MT2_STREAMING. Replaces MT2USBClaim + mt2_usb_read.c. */
class com_schmonz_MT2USBReader : public IOService {
    OSDeclareDefaultStructors(com_schmonz_MT2USBReader)
    IOUSBInterface *fIntf;
    IOUSBPipe *fPipe;
    IOBufferMemoryDescriptor *fBuf;
    uint16_t fMaxPacket;
    bool fStopping;
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    void armRead(void);
    static void readComplete(void *target, void *param, IOReturn status,
                             UInt32 bufferSizeRemaining);
};
#endif
